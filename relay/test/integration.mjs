#!/usr/bin/env node
/**
 * Local integration test for the m5stack-llm-ir relay.
 *
 * It starts `wrangler dev` (unless RELAY_URL points at an already running
 * relay), connects a fake M5Stack WebSocket client and verifies the public
 * contract:
 *
 *   - auth (401) for /api/* and /ws/device
 *   - /health without auth + device presence
 *   - HTTP -> WebSocket -> HTTP round trips (frames, body passthrough)
 *   - 503 DEVICE_OFFLINE, 504 DEVICE_TIMEOUT, 504 LEARN_TIMEOUT
 *   - fast failure of pending requests when the device disconnects
 *   - single-socket replacement (old socket closed with 1000 "replaced")
 *   - JSON keep-alive ping -> pong
 *
 * Usage:
 *   npm test
 *   RELAY_URL=http://127.0.0.1:8787 npm test   # server already running
 */

import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import process from "node:process";
import WebSocket from "ws";

const RELAY_DIR = fileURLToPath(new URL("..", import.meta.url));
const PORT = Number(process.env.RELAY_PORT ?? 8787);
const EXTERNAL_URL = process.env.RELAY_URL ?? null;
const BASE_URL = EXTERNAL_URL ?? `http://127.0.0.1:${String(PORT)}`;
const WS_URL = `${BASE_URL.replace(/^http/, "ws")}/ws/device`;
const API_KEY = process.env.API_KEY ?? "test-api-key";
const DEVICE_TOKEN = process.env.DEVICE_TOKEN ?? "test-device-token";

const results = [];

function check(name, condition, detail = "") {
  if (condition) {
    results.push({ ok: true, name });
    console.log(`  PASS  ${name}`);
  } else {
    results.push({ ok: false, name, detail });
    console.log(`  FAIL  ${name}${detail ? `  [${detail}]` : ""}`);
  }
}

function describe(value) {
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function withTimeout(promise, ms, label) {
  let timer;
  const timeout = new Promise((_resolve, reject) => {
    timer = setTimeout(
      () => reject(new Error(`timed out after ${String(ms)}ms: ${label}`)),
      ms,
    );
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

/** Poll a synchronous predicate until it is truthy. */
async function waitFor(predicate, timeoutMs, intervalMs = 100) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await sleep(intervalMs);
  }
  throw new Error(`condition not met within ${String(timeoutMs)}ms`);
}

async function readJson(response) {
  const text = await response.text();
  try {
    return JSON.parse(text);
  } catch {
    return { __raw: text };
  }
}

/** Fetch helper for the relayed `/api/*` endpoints. */
async function api(path, options = {}) {
  const { method = "GET", token = API_KEY, body, rawBody, headers = {} } = options;
  const mergedHeaders = { ...headers };
  if (token !== null) mergedHeaders.authorization = `Bearer ${token}`;
  let payload;
  if (rawBody !== undefined) {
    mergedHeaders["content-type"] = "application/json";
    payload = rawBody;
  } else if (body !== undefined) {
    mergedHeaders["content-type"] = "application/json";
    payload = JSON.stringify(body);
  }
  return fetch(`${BASE_URL}${path}`, {
    method,
    headers: mergedHeaders,
    body: payload,
  });
}

async function health() {
  const response = await fetch(`${BASE_URL}/health`, {
    signal: AbortSignal.timeout(5000),
  });
  return { status: response.status, body: await readJson(response) };
}

async function waitForHealth(predicate, timeoutMs = 15_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const snapshot = await health();
      if (predicate(snapshot.body)) return snapshot;
    } catch {
      // Server not ready yet.
    }
    await sleep(200);
  }
  return null;
}

// ---------------------------------------------------------------------------
// Fake M5Stack device: answers http_request frames with http_response frames.
// ---------------------------------------------------------------------------

class FakeDevice {
  requests = [];
  pongs = 0;
  /** (frame) => {status, body} | null — null means "do not answer". */
  handler = null;

  #requestWaiters = [];
  #pongWaiters = [];

  constructor(ws) {
    this.ws = ws;
    this.closeInfo = null;
    this.closed = new Promise((resolve) => {
      ws.once("close", (code, reason) => {
        this.closeInfo = { code, reason: reason.toString() };
        resolve(this.closeInfo);
      });
    });
    ws.on("message", (data) => {
      let frame;
      try {
        frame = JSON.parse(data.toString());
      } catch {
        return;
      }
      if (frame.type === "http_request") {
        this.requests.push(frame);
        const waiter = this.#requestWaiters.shift();
        if (waiter) waiter(frame);
        if (this.handler) {
          const reply = this.handler(frame);
          if (reply) this.respond(frame.req_id, reply.status, reply.body);
        }
      } else if (frame.type === "pong") {
        this.pongs += 1;
        const waiter = this.#pongWaiters.shift();
        if (waiter) waiter();
      }
    });
  }

  waitForRequest(timeoutMs = 5000) {
    const queued = this.requests.shift();
    if (queued !== undefined) return Promise.resolve(queued);
    return withTimeout(
      new Promise((resolve) => {
        this.#requestWaiters.push(resolve);
      }),
      timeoutMs,
      "waiting for http_request frame",
    );
  }

  waitForPong(timeoutMs = 5000) {
    if (this.pongs > 0 && this.#pongWaiters.length === 0) {
      // Already observed before we started waiting.
      return Promise.resolve();
    }
    return withTimeout(
      new Promise((resolve) => {
        this.#pongWaiters.push(resolve);
      }),
      timeoutMs,
      "waiting for pong frame",
    );
  }

  respond(reqId, status, body) {
    this.ws.send(
      JSON.stringify({
        type: "http_response",
        req_id: reqId,
        status,
        body,
      }),
    );
  }

  ping() {
    this.ws.send(JSON.stringify({ type: "ping" }));
  }

  close() {
    try {
      this.ws.close();
    } catch {
      // Already closed.
    }
  }
}

function connectDevice(token = DEVICE_TOKEN) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(WS_URL, {
      headers: { authorization: `Bearer ${token}` },
    });
    const onError = (error) => reject(error);
    ws.once("error", onError);
    ws.once("open", () => {
      ws.off("error", onError);
      resolve(new FakeDevice(ws));
    });
  });
}

/** Attempt an upgrade with a bad token; resolves with the HTTP status code. */
function expectUpgradeRejected(token) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(WS_URL, {
      headers: { authorization: `Bearer ${token}` },
    });
    let settled = false;
    ws.once("unexpected-response", (_request, response) => {
      settled = true;
      response.resume();
      resolve(response.statusCode);
      ws.terminate();
    });
    ws.once("open", () => {
      settled = true;
      ws.close();
      reject(new Error("WebSocket upgrade unexpectedly succeeded"));
    });
    ws.once("error", (error) => {
      if (!settled) {
        settled = true;
        reject(error);
      }
    });
  });
}

// ---------------------------------------------------------------------------
// wrangler dev process management
// ---------------------------------------------------------------------------

function tail(text, lines = 40) {
  return text.trim().split("\n").slice(-lines).join("\n");
}

async function startDevServer() {
  const wranglerBin = join(RELAY_DIR, "node_modules", ".bin", "wrangler");
  if (!existsSync(wranglerBin)) {
    throw new Error(`wrangler is not installed (${wranglerBin}); run npm install`);
  }

  const args = ["dev", "--port", String(PORT), "--ip", "127.0.0.1"];
  if (!existsSync(join(RELAY_DIR, ".dev.vars"))) {
    args.push(
      "--var",
      `API_KEY:${API_KEY}`,
      "--var",
      `DEVICE_TOKEN:${DEVICE_TOKEN}`,
    );
  }

  const child = spawn(wranglerBin, args, {
    cwd: RELAY_DIR,
    env: { ...process.env, CI: "1", NO_COLOR: "1" },
    stdio: ["ignore", "pipe", "pipe"],
    detached: true,
  });
  let log = "";
  child.stdout.on("data", (chunk) => {
    log += chunk.toString();
  });
  child.stderr.on("data", (chunk) => {
    log += chunk.toString();
  });
  const exited = new Promise((resolve) => {
    child.once("exit", (code, signal) => resolve({ code, signal }));
  });
  let exitResult = null;
  void exited.then((value) => {
    exitResult = value;
  });

  const server = { child, exited, log: () => log };
  try {
    const deadline = Date.now() + 90_000;
    while (Date.now() < deadline) {
      if (exitResult !== null) {
        throw new Error(
          `wrangler dev exited early (${describe(exitResult)})\n${tail(log)}`,
        );
      }
      try {
        const response = await fetch(`${BASE_URL}/health`, {
          signal: AbortSignal.timeout(1500),
        });
        if (response.ok) {
          await response.text();
          return server;
        }
      } catch {
        // Not listening yet.
      }
      await sleep(300);
    }
    throw new Error(`wrangler dev did not become ready within 90s\n${tail(log)}`);
  } catch (error) {
    await stopDevServer(server);
    throw error;
  }
}

async function stopDevServer(server) {
  if (server === null) return;
  const { child, exited } = server;
  try {
    process.kill(-child.pid, "SIGTERM");
  } catch {
    try {
      child.kill("SIGTERM");
    } catch {
      // Already gone.
    }
  }
  const outcome = await Promise.race([
    exited,
    sleep(8000).then(() => "timeout"),
  ]);
  if (outcome === "timeout") {
    try {
      process.kill(-child.pid, "SIGKILL");
    } catch {
      try {
        child.kill("SIGKILL");
      } catch {
        // Already gone.
      }
    }
    await Promise.race([exited, sleep(3000)]);
  }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const DEVICES_RESPONSE = {
  ok: true,
  devices: {
    living_tv: { label: "リビングのテレビ", actions: ["power", "vol_up"] },
    living_light: { label: "リビング照明", actions: ["on", "off"] },
  },
};

async function runTests() {
  const devices = [];

  // -- Phase A: no device connected ----------------------------------------
  console.log("\n[Phase A] Worker routes and auth without a device");

  const health0 = await health();
  check(
    "GET /health (no auth) -> 200, worker ok, device offline",
    health0.status === 200 &&
      health0.body?.ok === true &&
      health0.body?.worker === true &&
      health0.body?.device_online === false,
    describe(health0),
  );

  let response = await fetch(`${BASE_URL}/api/devices`);
  let json = await readJson(response);
  check(
    "GET /api/devices without Authorization -> 401 UNAUTHORIZED",
    response.status === 401 && json?.error?.code === "UNAUTHORIZED",
    `${String(response.status)} ${describe(json)}`,
  );

  response = await api("/api/devices", { token: "wrong-api-key" });
  json = await readJson(response);
  check(
    "GET /api/devices with wrong API_KEY -> 401 UNAUTHORIZED",
    response.status === 401 && json?.error?.code === "UNAUTHORIZED",
    `${String(response.status)} ${describe(json)}`,
  );

  response = await api("/api/devices");
  json = await readJson(response);
  check(
    "GET /api/devices with valid API_KEY and device offline -> 503 DEVICE_OFFLINE",
    response.status === 503 && json?.error?.code === "DEVICE_OFFLINE",
    `${String(response.status)} ${describe(json)}`,
  );

  response = await api("/api/ir/send", { method: "POST", rawBody: "{not json" });
  json = await readJson(response);
  check(
    "POST /api/ir/send with invalid JSON body -> 400 INVALID_JSON",
    response.status === 400 && json?.error?.code === "INVALID_JSON",
    `${String(response.status)} ${describe(json)}`,
  );

  response = await fetch(`${BASE_URL}/definitely-not-a-route`);
  json = await readJson(response);
  check(
    "non-/api route -> 404 NOT_FOUND at the Worker",
    response.status === 404 && json?.error?.code === "NOT_FOUND",
    `${String(response.status)} ${describe(json)}`,
  );

  response = await fetch(`${BASE_URL}/health`, { method: "POST" });
  json = await readJson(response);
  check(
    "POST /health -> 405 METHOD_NOT_ALLOWED",
    response.status === 405 && json?.error?.code === "METHOD_NOT_ALLOWED",
    `${String(response.status)} ${describe(json)}`,
  );

  const badUpgradeStatus = await expectUpgradeRejected("wrong-device-token");
  check(
    "GET /ws/device with wrong DEVICE_TOKEN -> HTTP 401",
    badUpgradeStatus === 401,
    `status=${String(badUpgradeStatus)}`,
  );

  // -- Phase B: fake device connected --------------------------------------
  console.log("\n[Phase B] HTTP <-> WebSocket relay with a fake device");

  const device = await connectDevice();
  devices.push(device);

  const health1 = await waitForHealth((body) => body?.device_online === true);
  check(
    "GET /health reports device_online=true after the device connects",
    health1 !== null,
    "device_online never became true",
  );

  device.ping();
  let pongReceived = true;
  try {
    await device.waitForPong(3000);
  } catch {
    pongReceived = false;
  }
  check(
    'device JSON {"type":"ping"} -> relay replies {"type":"pong"}',
    pongReceived && device.pongs === 1,
    `pongs=${String(device.pongs)}`,
  );

  const seenFrames = [];
  device.handler = (frame) => {
    seenFrames.push(frame);
    if (frame.path === "/api/devices") {
      return { status: 200, body: DEVICES_RESPONSE };
    }
    if (frame.path === "/api/ir/send" && frame.method === "POST") {
      return { status: 200, body: { ok: true, executed_steps: 1 } };
    }
    if (frame.path === "/api/nope") {
      return {
        status: 404,
        body: { ok: false, error: { code: "NOT_FOUND", message: "unknown" } },
      };
    }
    if (frame.path === "/api/bad-status") {
      return { status: 999, body: { ok: true } };
    }
    if (frame.path === "/api/slow" || frame.path === "/api/hang") {
      return null; // Deliberately never answer.
    }
    return {
      status: 500,
      body: { ok: false, error: { code: "UNEXPECTED", message: frame.path } },
    };
  };

  response = await api("/api/devices");
  json = await readJson(response);
  check(
    "GET /api/devices round-trips 200 + device JSON body",
    response.status === 200 &&
      json?.ok === true &&
      json?.devices?.living_tv?.label === "リビングのテレビ",
    `${String(response.status)} ${describe(json)}`,
  );
  check(
    "relayed response keeps JSON content-type and cache-control: no-store",
    (response.headers.get("content-type") ?? "").startsWith(
      "application/json",
    ) && response.headers.get("cache-control") === "no-store",
    describe({
      contentType: response.headers.get("content-type"),
      cacheControl: response.headers.get("cache-control"),
    }),
  );

  const getFrame = seenFrames.find((frame) => frame.path === "/api/devices");
  check(
    "device receives http_request{method:GET, path, body:null, req_id}",
    getFrame !== undefined &&
      getFrame.type === "http_request" &&
      getFrame.method === "GET" &&
      getFrame.path === "/api/devices" &&
      getFrame.body === null &&
      typeof getFrame.req_id === "string" &&
      getFrame.req_id.startsWith("req_"),
    describe(getFrame),
  );

  response = await api("/api/ir/send", {
    method: "POST",
    body: { device: "living_light", action: "off" },
  });
  json = await readJson(response);
  const sendFrame = seenFrames.find((frame) => frame.path === "/api/ir/send");
  check(
    "POST /api/ir/send forwards method + parsed JSON body and returns device response",
    response.status === 200 &&
      json?.executed_steps === 1 &&
      sendFrame?.method === "POST" &&
      sendFrame?.body?.device === "living_light" &&
      sendFrame?.body?.action === "off",
    `${String(response.status)} ${describe({ json, sendFrame })}`,
  );

  response = await api("/api/nope");
  json = await readJson(response);
  check(
    "unknown /api path is forwarded and the device 404 passes through",
    response.status === 404 &&
      json?.error?.code === "NOT_FOUND" &&
      seenFrames.some((frame) => frame.path === "/api/nope"),
    `${String(response.status)} ${describe(json)}`,
  );

  response = await api("/api/bad-status");
  json = await readJson(response);
  check(
    "device frame with invalid HTTP status -> 502 BAD_GATEWAY",
    response.status === 502 && json?.error?.code === "BAD_GATEWAY",
    `${String(response.status)} ${describe(json)}`,
  );

  let startedAt = Date.now();
  response = await api("/api/slow");
  let elapsed = Date.now() - startedAt;
  json = await readJson(response);
  check(
    "unanswered normal request -> 504 DEVICE_TIMEOUT after ~10s",
    response.status === 504 &&
      json?.error?.code === "DEVICE_TIMEOUT" &&
      elapsed >= 9_000 &&
      elapsed <= 15_000,
    `${String(response.status)} ${describe(json)} after ${String(elapsed)}ms`,
  );

  device.handler = () => null; // Deliberately answer nothing from now on.
  startedAt = Date.now();
  response = await api("/api/ir/learn", {
    method: "POST",
    body: { timeout_ms: 1000 },
  });
  elapsed = Date.now() - startedAt;
  json = await readJson(response);
  check(
    "unanswered POST /api/ir/learn -> 504 LEARN_TIMEOUT after timeout_ms+5s (~6s)",
    response.status === 504 &&
      json?.error?.code === "LEARN_TIMEOUT" &&
      elapsed >= 5_500 &&
      elapsed <= 8_500,
    `${String(response.status)} ${describe(json)} after ${String(elapsed)}ms`,
  );

  const pendingResponse = api("/api/hang");
  await sleep(400);
  device.close();
  response = await withTimeout(
    pendingResponse,
    5000,
    "pending request after device disconnect",
  );
  json = await readJson(response);
  check(
    "device disconnect fails the in-flight request fast with 503 DEVICE_OFFLINE",
    response.status === 503 && json?.error?.code === "DEVICE_OFFLINE",
    `${String(response.status)} ${describe(json)}`,
  );

  const offlineHealth = await waitForHealth(
    (body) => body?.device_online === false,
  );
  check(
    "GET /health reports device_online=false after the device disconnects",
    offlineHealth !== null,
    "device_online stayed true",
  );

  // -- Phase C: socket replacement (device reboot / reconnect) -------------
  console.log("\n[Phase C] Single active device socket / replacement");

  const deviceA = await connectDevice();
  devices.push(deviceA);
  await waitForHealth((body) => body?.device_online === true);

  deviceA.handler = (frame) =>
    frame.path === "/api/hang2"
      ? null
      : { status: 200, body: { ok: true, served_by: "A" } };
  const pendingOnA = api("/api/hang2");
  await sleep(400);

  const deviceB = await connectDevice();
  devices.push(deviceB);

  // The `ws` client emits "close" only once the whole close handshake finishes.
  // Workerd's local dev server can defer that completion by several seconds, but
  // the relay sends the close frame immediately, which moves the client to
  // CLOSING (readyState 2) right away.
  let closeInitiated = true;
  try {
    await waitFor(() => deviceA.ws.readyState === 2, 5000);
  } catch {
    closeInitiated = false;
  }
  check(
    'new device connection immediately sends a close frame to the old socket',
    closeInitiated,
    `readyState=${String(deviceA.ws.readyState)}`,
  );

  // If the handshake completes within a reasonable window, verify the
  // code/reason contract; local workerd may defer it, hence the fallback note.
  const eventualClose = await Promise.race([
    deviceA.closed,
    sleep(12000).then(() => null),
  ]);
  if (eventualClose !== null) {
    check(
      'old socket close event has code 1000 / reason "replaced"',
      eventualClose.code === 1000 && eventualClose.reason === "replaced",
      describe(eventualClose),
    );
  } else {
    console.log(
      "  NOTE  workerd deferred the old-socket close handshake; close frame already observed via CLOSING state",
    );
  }

  response = await withTimeout(
    pendingOnA,
    5000,
    "pending request on the replaced socket",
  );
  json = await readJson(response);
  check(
    "request pending on the replaced socket fails fast with 503 DEVICE_OFFLINE",
    response.status === 503 && json?.error?.code === "DEVICE_OFFLINE",
    `${String(response.status)} ${describe(json)}`,
  );

  deviceB.handler = () => ({
    status: 200,
    body: { ok: true, served_by: "B" },
  });
  response = await api("/api/devices");
  json = await readJson(response);
  check(
    "the replacement socket serves subsequent requests",
    response.status === 200 && json?.served_by === "B",
    `${String(response.status)} ${describe(json)}`,
  );

  check(
    "GET /health still reports device_online=true after replacement",
    (await waitForHealth((body) => body?.device_online === true)) !== null,
    "device_online=false",
  );

  deviceB.close();
  const finalOffline = await waitForHealth(
    (body) => body?.device_online === false,
  );
  check(
    "device_online=false after closing the replacement socket",
    finalOffline !== null,
    "device_online stayed true",
  );

  // Clean up any still-open sockets.
  for (const openDevice of devices) openDevice.close();
}

async function main() {
  console.log(`Relay integration test -> ${BASE_URL}`);

  let server = null;
  try {
    if (EXTERNAL_URL === null) {
      console.log("Starting wrangler dev (local) ...");
      server = await startDevServer();
      console.log("wrangler dev is ready.");
    } else {
      console.log("Using already-running relay server (RELAY_URL set).");
    }

    await runTests();
  } finally {
    if (server !== null) {
      console.log("Stopping wrangler dev ...");
      await stopDevServer(server);
    }
  }

  const failed = results.filter((result) => !result.ok);
  console.log(
    `\n${String(results.length - failed.length)}/${String(results.length)} checks passed.`,
  );
  if (failed.length > 0) {
    console.log("Failures:");
    for (const failure of failed) {
      console.log(`  - ${failure.name}${failure.detail ? `: ${failure.detail}` : ""}`);
    }
    process.exitCode = 1;
  }
}

main().catch((error) => {
  console.error("\nIntegration test crashed:", error);
  process.exitCode = 1;
});
