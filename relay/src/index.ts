/**
 * m5stack-llm-ir relay
 * ====================
 *
 * Optional Cloudflare Workers + Durable Objects reverse tunnel that exposes the
 * M5Stack IR remote's local HTTP API on the public internet without opening any
 * router ports:
 *
 *   LLM agent --HTTPS+API_KEY--> Worker --(Durable Object call)--> Relay DO
 *                                                                     |
 *                                                          WebSocket (device pulls)
 *                                                                     v
 *                                                              M5Stack firmware
 *
 * Public routes handled by the Worker entry point:
 *
 *   GET /health      no auth. Worker liveness + device presence.
 *   GET /ws/device   WebSocket upgrade from the M5Stack.
 *                    Authorization: Bearer <DEVICE_TOKEN>
 *   *   /api/*       Relayed agent API (same JSON contract as the local API).
 *                    Authorization: Bearer <API_KEY> is verified here, before
 *                    the Durable Object is contacted.
 *
 * Any other path 404s at the Worker. Unknown `/api/*` paths are forwarded to
 * the device, which is the authority on its own API and will return 404.
 *
 * The Durable Object uses the WebSocket Hibernation API so the relay is free to
 * run on the Workers Free plan while the device is idle.
 */

import {
  isJsonObject,
  type Env,
  type HttpRequestFrame,
  type JsonValue,
  type RelayHttpRequestEnvelope,
} from "./types";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const JSON_CONTENT_TYPE = "application/json; charset=utf-8";
const NO_STORE = "no-store";

/**
 * The static test GUI is served from GitHub Pages (https://<user>.github.io),
 * so the relay must be callable cross-origin from the browser:
 *   - `OPTIONS` preflight requests are answered before authentication.
 *   - Every response gets the CORS headers below.
 */
const CORS_HEADERS: Record<string, string> = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET, POST, DELETE, OPTIONS",
  "access-control-allow-headers": "Authorization, Content-Type",
  "access-control-max-age": "86400",
};

function withCors(response: Response): Response {
  const headers = new Headers(response.headers);
  for (const [name, value] of Object.entries(CORS_HEADERS)) {
    headers.set(name, value);
  }
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

/** Timeout for ordinary relayed HTTP requests (spec 6.3). */
const NORMAL_REQUEST_TIMEOUT_MS = 10_000;

/** `/api/ir/learn` gets `timeout_ms + 5s`, capped at 35s (spec 6.3). */
const LEARN_TIMEOUT_GRACE_MS = 5_000;
const LEARN_DEFAULT_TIMEOUT_MS = 10_000;
const LEARN_MAX_TIMEOUT_MS = 30_000;
const LEARN_PATH = "/api/ir/learn";

/** Singleton Durable Object name: one M5Stack, one relay instance. */
const RELAY_DO_NAME = "relay";

/** Internal URLs used only across the `RELAY_DO` binding (never public). */
const INTERNAL_HEALTH_URL = "https://relay.internal/health";
const INTERNAL_RELAY_URL = "https://relay.internal/relay";

const DEVICE_WS_PATH = "/ws/device";

/**
 * `WebSocket.readyState` for an OPEN socket. A socket that was just replaced
 * may linger in CLOSING state for a short while; it must never be treated as
 * the active device.
 */
const WS_READY_STATE_OPEN = 1;

// ---------------------------------------------------------------------------
// Small HTTP helpers
// ---------------------------------------------------------------------------

function jsonResponse(status: number, payload: unknown): Response {
  // 204/205/304 must not carry a body.
  if (status === 204 || status === 205 || status === 304) {
    return new Response(null, {
      status,
      headers: {
        "content-type": JSON_CONTENT_TYPE,
        "cache-control": NO_STORE,
      },
    });
  }
  return new Response(JSON.stringify(payload ?? null), {
    status,
    headers: {
      "content-type": JSON_CONTENT_TYPE,
      "cache-control": NO_STORE,
    },
  });
}

function errorResponse(status: number, code: string, message: string): Response {
  return jsonResponse(status, { ok: false, error: { code, message } });
}

function readBearerToken(request: Request): string | null {
  const header = request.headers.get("authorization");
  if (header === null) return null;
  const match = /^Bearer\s+(.+)$/i.exec(header.trim());
  const token = match?.[1]?.trim();
  return token ? token : null;
}

function isAuthorized(request: Request, expectedSecret: string): boolean {
  // Fail closed when the secret is not configured.
  if (!expectedSecret) return false;
  const token = readBearerToken(request);
  return token !== null && token === expectedSecret;
}

/** Read the request body as JSON. Empty body -> null. Throws on invalid JSON. */
async function readJsonBody(request: Request): Promise<JsonValue | null> {
  const raw = await request.text();
  if (raw.trim() === "") return null;
  return JSON.parse(raw) as JsonValue;
}

function getRelayStub(env: Env): DurableObjectStub {
  const id = env.RELAY_DO.idFromName(RELAY_DO_NAME);
  return env.RELAY_DO.get(id);
}

function logError(scope: string, error: unknown): void {
  // Never log secrets (API_KEY / DEVICE_TOKEN are not part of these messages).
  const message = error instanceof Error ? error.message : String(error);
  console.error(`[relay] ${scope}: ${message}`);
}

// ---------------------------------------------------------------------------
// Worker entry point
// ---------------------------------------------------------------------------

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    // CORS preflight must never require authentication.
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: CORS_HEADERS });
    }
    const response = await handleWorkerRequest(request, env);
    return withCors(response);
  },
} satisfies ExportedHandler<Env>;

async function handleWorkerRequest(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;

    if (path === "/health") {
      if (request.method !== "GET") {
        return errorResponse(405, "METHOD_NOT_ALLOWED", "Use GET /health.");
      }
      return handleHealth(env);
    }

    if (path === DEVICE_WS_PATH) {
      if (request.method !== "GET") {
        return errorResponse(405, "METHOD_NOT_ALLOWED", "Use GET /ws/device.");
      }
      return handleDeviceWebSocket(request, env);
    }

    if (path === "/api" || path.startsWith("/api/")) {
      return handleRelayedApiRequest(request, env);
    }

    return errorResponse(
      404,
      "NOT_FOUND",
      `No relay route for ${request.method} ${path}.`,
    );
}

/** `GET /health` — unauthenticated liveness probe that also reports presence. */
async function handleHealth(env: Env): Promise<Response> {
  let deviceOnline = false;
  try {
    const doResponse = await getRelayStub(env).fetch(
      new Request(INTERNAL_HEALTH_URL, { method: "GET" }),
    );
    if (doResponse.ok) {
      const payload = (await doResponse.json()) as {
        device_online?: unknown;
      };
      deviceOnline = payload.device_online === true;
    }
  } catch (error) {
    // The Worker itself is still healthy even if the Durable Object is not.
    logError("health check failed", error);
  }

  return jsonResponse(200, {
    ok: true,
    worker: true,
    device_online: deviceOnline,
  });
}

/** `GET /ws/device` — authenticate the M5Stack and hand the upgrade to the DO. */
async function handleDeviceWebSocket(
  request: Request,
  env: Env,
): Promise<Response> {
  if (!isAuthorized(request, env.DEVICE_TOKEN)) {
    return errorResponse(
      401,
      "UNAUTHORIZED",
      "Invalid or missing device token.",
    );
  }

  const upgrade = request.headers.get("upgrade");
  if (upgrade === null || upgrade.toLowerCase() !== "websocket") {
    return errorResponse(
      400,
      "BAD_REQUEST",
      "Expected a WebSocket upgrade request.",
    );
  }

  try {
    // The Durable Object creates the WebSocketPair and returns a 101 response;
    // the Worker passes that response straight back to the device.
    return await getRelayStub(env).fetch(request);
  } catch (error) {
    logError("device websocket upgrade failed", error);
    return errorResponse(
      502,
      "BAD_GATEWAY",
      "The relay Durable Object is unavailable.",
    );
  }
}

/**
 * `* /api/*` — verify the agent API key, then relay the request through the
 * Durable Object. Authentication happens strictly before the DO is contacted
 * so unauthenticated traffic can never reach (or wake) the relay.
 */
async function handleRelayedApiRequest(
  request: Request,
  env: Env,
): Promise<Response> {
  if (!isAuthorized(request, env.API_KEY)) {
    return errorResponse(401, "UNAUTHORIZED", "Invalid or missing API key.");
  }

  const url = new URL(request.url);

  let body: JsonValue | null;
  try {
    body = await readJsonBody(request);
  } catch {
    return errorResponse(
      400,
      "INVALID_JSON",
      "Request body must be valid JSON (or empty).",
    );
  }

  const envelope: RelayHttpRequestEnvelope = {
    method: request.method.toUpperCase(),
    // The MVP API has no query strings; the path alone is the device contract.
    path: url.pathname,
    body,
  };

  try {
    return await getRelayStub(env).fetch(
      new Request(INTERNAL_RELAY_URL, {
        method: "POST",
        headers: { "content-type": JSON_CONTENT_TYPE },
        body: JSON.stringify(envelope),
      }),
    );
  } catch (error) {
    logError("relayed request failed", error);
    return errorResponse(
      502,
      "BAD_GATEWAY",
      "The relay Durable Object is unavailable.",
    );
  }
}

// ---------------------------------------------------------------------------
// Durable Object: one per M5Stack, holds the hibernatable device WebSocket
// ---------------------------------------------------------------------------

interface PendingRequest {
  /** Resolves the HTTP request that is waiting for the device answer. */
  resolve: (response: Response) => void;
  /** setTimeout id used for the DEVICE_TIMEOUT / LEARN_TIMEOUT guard. */
  timer: ReturnType<typeof setTimeout>;
}

interface RelayTimeout {
  ms: number;
  code: "DEVICE_TIMEOUT" | "LEARN_TIMEOUT";
  message: string;
}

export class Relay implements DurableObject {
  /**
   * In-flight requests, keyed by `req_id`. This only ever holds requests while
   * the DO is awake: an in-flight `fetch()` keeps the object alive, so a
   * hibernated DO always wakes with an empty map. Timers are intentionally
   * in-memory for the same reason.
   */
  private readonly pending = new Map<string, PendingRequest>();

  constructor(
    private readonly ctx: DurableObjectState,
    private readonly _env: Env,
  ) {}

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const upgrade = request.headers.get("upgrade");

    if (
      url.pathname === DEVICE_WS_PATH ||
      (upgrade !== null && upgrade.toLowerCase() === "websocket")
    ) {
      return this.handleDeviceConnection();
    }

    if (request.method === "GET" && url.pathname === "/health") {
      return jsonResponse(200, {
        ok: true,
        device_online: this.isDeviceOnline(),
      });
    }

    if (request.method === "POST" && url.pathname === "/relay") {
      return this.handleRelayedRequest(request);
    }

    return errorResponse(404, "NOT_FOUND", "Unknown relay internal route.");
  }

  // -- Device WebSocket lifecycle -------------------------------------------

  /**
   * Accept a new M5Stack connection. There is exactly one active device
   * socket: a rebooted/reconnected device replaces (and closes) older sockets
   * so ghost connections cannot linger. Requests still waiting on the old
   * socket can never be answered by the new one, so they are failed fast with
   * 503 DEVICE_OFFLINE instead of waiting for a timeout.
   */
  private handleDeviceConnection(): Response {
    const previousSockets = this.ctx.getWebSockets();
    if (previousSockets.length > 0) {
      this.rejectAllPending(
        "DEVICE_OFFLINE",
        "The M5Stack reconnected before the request could be answered.",
      );
    }
    for (const previous of previousSockets) {
      try {
        previous.close(1000, "replaced");
      } catch {
        // Already closing / closed; nothing else to do.
      }
    }

    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    // Hibernation API: the runtime owns the socket and wakes us on messages.
    this.ctx.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  private isDeviceOnline(): boolean {
    return this.getOpenDeviceSockets().length > 0;
  }

  /** Currently OPEN device sockets (closing sockets are excluded). */
  private getOpenDeviceSockets(): WebSocket[] {
    return this.ctx
      .getWebSockets()
      .filter((socket) => socket.readyState === WS_READY_STATE_OPEN);
  }

  private getActiveDeviceSocket(): WebSocket | null {
    const sockets = this.getOpenDeviceSockets();
    const active = sockets[sockets.length - 1];
    return active ?? null;
  }

  /**
   * Called from `webSocketClose` / `webSocketError`. If another device socket
   * is still connected, this one was simply replaced and must not tear down
   * the relay; otherwise the device is gone and every pending request fails
   * fast with 503 DEVICE_OFFLINE.
   */
  private handleDeviceSocketLoss(closedSocket: WebSocket): void {
    const remaining = this.getOpenDeviceSockets().filter(
      (socket) => socket !== closedSocket,
    );
    if (remaining.length > 0) return;

    this.rejectAllPending(
      "DEVICE_OFFLINE",
      "The M5Stack disconnected before the request could be answered.",
    );
  }

  // -- WebSocket Hibernation API callbacks ----------------------------------

  webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): void {
    // The device protocol is JSON text frames only; ignore binary frames.
    if (typeof message !== "string") return;

    let parsed: unknown;
    try {
      parsed = JSON.parse(message);
    } catch {
      return; // Malformed JSON cannot be matched to a request; drop it.
    }
    if (!isJsonObject(parsed)) return;

    switch (parsed["type"]) {
      case "ping":
        this.sendToSocket(ws, JSON.stringify({ type: "pong" }));
        return;
      case "http_response":
        this.handleDeviceResponse(parsed);
        return;
      default:
        // "pong" and any future/unknown frame types are ignored gracefully.
        return;
    }
  }

  webSocketClose(
    ws: WebSocket,
    _code: number,
    _reason: string,
    _wasClean: boolean,
  ): void {
    this.handleDeviceSocketLoss(ws);
  }

  webSocketError(ws: WebSocket, _error: unknown): void {
    this.handleDeviceSocketLoss(ws);
  }

  private sendToSocket(ws: WebSocket, payload: string): void {
    try {
      ws.send(payload);
    } catch {
      // The close/error callback will clean up presence and pending requests.
    }
  }

  /**
   * Handle `{"type":"http_response","req_id","status","body"}` from the device.
   * The status and JSON body are returned to the waiting agent verbatim.
   */
  private handleDeviceResponse(frame: Record<string, JsonValue>): void {
    const reqId = frame["req_id"];
    if (typeof reqId !== "string") return;
    if (!this.pending.has(reqId)) return; // Timed out or already answered.

    const status = frame["status"];
    if (
      typeof status !== "number" ||
      !Number.isInteger(status) ||
      status < 200 ||
      status > 599
    ) {
      this.settleRequest(reqId, () =>
        errorResponse(
          502,
          "BAD_GATEWAY",
          "The M5Stack returned an invalid HTTP status.",
        ),
      );
      return;
    }

    const body = frame["body"] ?? null;
    this.settleRequest(reqId, () => jsonResponse(status, body));
  }

  // -- Forwarding agent HTTP requests to the device -------------------------

  private async handleRelayedRequest(request: Request): Promise<Response> {
    let envelope: RelayHttpRequestEnvelope;
    try {
      const parsed = (await request.json()) as unknown;
      if (
        !isJsonObject(parsed) ||
        typeof parsed["method"] !== "string" ||
        typeof parsed["path"] !== "string"
      ) {
        return errorResponse(
          400,
          "BAD_REQUEST",
          "Malformed relay request envelope.",
        );
      }
      envelope = {
        method: parsed["method"],
        path: parsed["path"],
        body: parsed["body"] ?? null,
      };
    } catch {
      return errorResponse(
        400,
        "BAD_REQUEST",
        "Malformed relay request envelope.",
      );
    }

    const socket = this.getActiveDeviceSocket();
    if (socket === null) {
      return errorResponse(
        503,
        "DEVICE_OFFLINE",
        "The M5Stack is not connected to the relay.",
      );
    }

    const reqId = `req_${crypto.randomUUID()}`;
    const timeout = computeTimeout(
      envelope.method,
      envelope.path,
      envelope.body,
    );
    const frame: HttpRequestFrame = {
      type: "http_request",
      req_id: reqId,
      method: envelope.method.toUpperCase(),
      path: envelope.path,
      body: envelope.body,
    };

    const responsePromise = new Promise<Response>((resolve) => {
      const timer = setTimeout(() => {
        this.pending.delete(reqId);
        resolve(errorResponse(504, timeout.code, timeout.message));
      }, timeout.ms);
      this.pending.set(reqId, { resolve, timer });
    });

    try {
      socket.send(JSON.stringify(frame));
    } catch {
      this.cancelPending(reqId);
      return errorResponse(
        503,
        "DEVICE_OFFLINE",
        "Failed to deliver the request to the M5Stack.",
      );
    }

    return await responsePromise;
  }

  private settleRequest(reqId: string, build: () => Response): void {
    const pendingRequest = this.pending.get(reqId);
    if (pendingRequest === undefined) return;
    this.pending.delete(reqId);
    clearTimeout(pendingRequest.timer);
    pendingRequest.resolve(build());
  }

  private cancelPending(reqId: string): void {
    const pendingRequest = this.pending.get(reqId);
    if (pendingRequest === undefined) return;
    this.pending.delete(reqId);
    clearTimeout(pendingRequest.timer);
  }

  private rejectAllPending(code: string, message: string): void {
    if (this.pending.size === 0) return;
    const pendingRequests = [...this.pending.values()];
    this.pending.clear();
    for (const pendingRequest of pendingRequests) {
      clearTimeout(pendingRequest.timer);
      pendingRequest.resolve(errorResponse(503, code, message));
    }
  }
}

// ---------------------------------------------------------------------------
// Timeout policy (spec 6.3)
// ---------------------------------------------------------------------------

function normalizePath(path: string): string {
  return path.length > 1 ? path.replace(/\/+$/, "") : path;
}

/**
 * `POST /api/ir/learn` waits `timeout_ms + 5s`, capped at 35s.
 * `timeout_ms` itself is clamped to `1..30000`; absent/invalid -> 10000.
 */
function getLearnTimeoutMs(body: JsonValue | null): number {
  if (!isJsonObject(body)) return LEARN_DEFAULT_TIMEOUT_MS;
  const raw = body["timeout_ms"];
  if (typeof raw !== "number" || !Number.isFinite(raw)) {
    return LEARN_DEFAULT_TIMEOUT_MS;
  }
  return Math.min(Math.max(Math.floor(raw), 1), LEARN_MAX_TIMEOUT_MS);
}

function computeTimeout(
  method: string,
  path: string,
  body: JsonValue | null,
): RelayTimeout {
  const isLearn =
    method.toUpperCase() === "POST" && normalizePath(path) === LEARN_PATH;

  if (isLearn) {
    const learnMs = getLearnTimeoutMs(body);
    const totalMs = learnMs + LEARN_TIMEOUT_GRACE_MS;
    return {
      ms: totalMs,
      code: "LEARN_TIMEOUT",
      message: `The M5Stack did not answer the learn request within ${totalMs} ms.`,
    };
  }

  return {
    ms: NORMAL_REQUEST_TIMEOUT_MS,
    code: "DEVICE_TIMEOUT",
    message: `The M5Stack did not answer within ${NORMAL_REQUEST_TIMEOUT_MS} ms.`,
  };
}
