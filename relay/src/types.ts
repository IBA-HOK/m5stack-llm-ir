/**
 * Shared types for the m5stack-llm-ir Cloudflare Workers relay.
 *
 * Wire protocol between the Relay Durable Object and the M5Stack
 * (see `spec/SPEC.md` section 6.2):
 *
 *   Relay   -> M5Stack : {"type":"http_request","req_id","method","path","body"}
 *   M5Stack -> Relay   : {"type":"http_response","req_id","status","body"}
 *   M5Stack -> Relay   : {"type":"ping"}   (the relay answers {"type":"pong"})
 */

/** Any value that can survive `JSON.parse` / `JSON.stringify`. */
export type JsonValue =
  | null
  | boolean
  | number
  | string
  | JsonValue[]
  | { [key: string]: JsonValue };

/**
 * Internal envelope sent from the Worker entry point to the Relay Durable
 * Object over the `RELAY_DO` binding. The Worker has already verified the
 * API key and parsed the JSON body at this point.
 */
export interface RelayHttpRequestEnvelope {
  method: string;
  path: string;
  body: JsonValue | null;
}

/** Frame the relay sends to the M5Stack. */
export interface HttpRequestFrame {
  type: "http_request";
  req_id: string;
  method: string;
  path: string;
  body: JsonValue | null;
}

/** Frame the M5Stack sends back to the relay. */
export interface HttpResponseFrame {
  type: "http_response";
  req_id: string;
  status: number;
  body?: JsonValue | null;
}

export interface PingFrame {
  type: "ping";
}

export interface PongFrame {
  type: "pong";
}

export type RelayToDeviceFrame = HttpRequestFrame | PongFrame;

/** Narrow `unknown` to a plain JSON object (not null, not an array). */
export function isJsonObject(value: unknown): value is Record<string, JsonValue> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** Environment bindings / secrets of this Worker and its Durable Object. */
export interface Env {
  /** Durable Object namespace for the single `Relay` instance. */
  RELAY_DO: DurableObjectNamespace;
  /** Agent <-> relay HTTP API key (`npx wrangler secret put API_KEY`). */
  API_KEY: string;
  /** M5Stack <-> relay WebSocket token (`npx wrangler secret put DEVICE_TOKEN`). */
  DEVICE_TOKEN: string;
}
