#include "relay_ws.h"

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "api_handler.h"
#include "config.h"
#include "state.h"

namespace relayws {
namespace {

struct RelayEndpoint {
  bool configured = false;
  bool tls = true;
  String host;
  uint16_t port = 443;
  String path;
};

struct RelayFrame {
  String payload;
};

RelayEndpoint s_endpoint;
WebSocketsClient s_ws;
String s_extraHeaders;

bool s_started = false;
bool s_connected = false;
bool s_reconnectPending = false;
bool s_attemptInFlight = false;
uint32_t s_attemptStartedMs = 0;
uint32_t s_nextAttemptMs = 0;
uint32_t s_backoffMs = M5IR_RELAY_BACKOFF_MIN_MS;
uint32_t s_lastPingMs = 0;
uint32_t s_lastPongMs = 0;

QueueHandle_t s_requestQueue = nullptr;
QueueHandle_t s_responseQueue = nullptr;

void scheduleReconnect() {
  if (!s_endpoint.configured) return;
  if (s_connected) {
    Serial.println("[relay] disconnected");
  }
  s_connected = false;
  s_attemptInFlight = false;
  appstate::setRelayStatus(true, false);
  if (s_reconnectPending) return;  // already scheduled

  s_reconnectPending = true;
  s_nextAttemptMs = millis() + s_backoffMs;
  Serial.printf("[relay] reconnecting in %u ms\n", static_cast<unsigned>(s_backoffMs));
  const uint32_t next = (s_backoffMs < M5IR_RELAY_BACKOFF_MAX_MS / 2) ? s_backoffMs * 2
                                                                     : M5IR_RELAY_BACKOFF_MAX_MS;
  s_backoffMs = (next > M5IR_RELAY_BACKOFF_MAX_MS) ? M5IR_RELAY_BACKOFF_MAX_MS : next;
}

void attemptConnect() {
  s_reconnectPending = false;
  s_attemptInFlight = true;
  s_attemptStartedMs = millis();
  Serial.printf("[relay] connecting to %s://%s:%u%s\n", s_endpoint.tls ? "wss" : "ws",
                s_endpoint.host.c_str(), static_cast<unsigned>(s_endpoint.port),
                s_endpoint.path.c_str());
  if (s_endpoint.tls) {
    s_ws.beginSSL(s_endpoint.host.c_str(), s_endpoint.port, s_endpoint.path.c_str());
  } else {
    s_ws.begin(s_endpoint.host.c_str(), s_endpoint.port, s_endpoint.path.c_str());
  }
}

void enqueueBusyReply(const String& requestId) {
  if (s_responseQueue == nullptr) return;
  JsonDocument doc;
  doc["type"] = "http_response";
  doc["req_id"] = requestId;
  doc["status"] = 503;
  JsonObject body = doc["body"].to<JsonObject>();
  body["ok"] = false;
  JsonObject error = body["error"].to<JsonObject>();
  error["code"] = "DEVICE_BUSY";
  error["message"] = "the device relay worker queue is full";
  String* payload = new String();
  serializeJson(doc, *payload);
  if (xQueueSend(s_responseQueue, &payload, 0) != pdTRUE) delete payload;
}

void handleTextFrame(const uint8_t* payload, size_t length) {
  JsonDocument doc;
  const DeserializationError error =
      deserializeJson(doc, reinterpret_cast<const char*>(payload), length);
  if (error) {
    Serial.printf("[relay] invalid JSON frame: %s\n", error.c_str());
    return;
  }

  const char* type = doc["type"] | "";
  if (strcmp(type, "pong") == 0) {
    s_lastPongMs = millis();
    return;
  }
  if (strcmp(type, "ping") == 0) {
    s_ws.sendTXT("{\"type\":\"pong\"}");
    return;
  }
  if (strcmp(type, "http_request") != 0) {
    return;
  }

  const String requestId = String(doc["req_id"] | "");
  if (requestId.isEmpty() || s_requestQueue == nullptr) {
    enqueueBusyReply(requestId);
    return;
  }

  RelayFrame* frame = new RelayFrame();
  if (frame == nullptr) {
    enqueueBusyReply(requestId);
    return;
  }
  frame->payload.reserve(length + 1);
  frame->payload.concat(reinterpret_cast<const char*>(payload), length);
  if (xQueueSend(s_requestQueue, &frame, 0) != pdTRUE) {
    delete frame;
    enqueueBusyReply(requestId);
  }
}

void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[relay] connected");
      s_connected = true;
      s_reconnectPending = false;
      s_attemptInFlight = false;
      s_backoffMs = M5IR_RELAY_BACKOFF_MIN_MS;
      s_lastPingMs = millis();
      s_lastPongMs = millis();
      appstate::setRelayStatus(true, true);
      break;
    case WStype_DISCONNECTED:
      Serial.printf("[relay] disconnected (code=%u reason=%s)\n",
                    static_cast<unsigned>(length),
                    payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
      scheduleReconnect();
      break;
    case WStype_ERROR:
      Serial.printf("[relay] socket error (len=%u): %s\n", static_cast<unsigned>(length),
                    payload != nullptr ? reinterpret_cast<const char*>(payload) : "");
      scheduleReconnect();
      break;
    case WStype_TEXT:
      handleTextFrame(payload, length);
      break;
    default:
      break;
  }
}

// Executes relayed HTTP requests. Runs in its own FreeRTOS task so that slow
// IR operations never block the UI/network main loop.
void workerTask(void* argument) {
  (void)argument;
  for (;;) {
    RelayFrame* frame = nullptr;
    if (xQueueReceive(s_requestQueue, &frame, portMAX_DELAY) != pdTRUE || frame == nullptr) {
      continue;
    }

    JsonDocument frameDoc;
    const DeserializationError frameError = deserializeJson(frameDoc, frame->payload);
    String requestId;
    ApiRequest request;
    ApiResponse response;

    if (frameError) {
      requestId = "";
      response.status = 400;
      JsonDocument errorDoc;
      errorDoc["ok"] = false;
      JsonObject error = errorDoc["error"].to<JsonObject>();
      error["code"] = "INVALID_JSON";
      error["message"] = "relay frame is not valid JSON";
      serializeJson(errorDoc, response.body);
    } else {
      requestId = String(frameDoc["req_id"] | "");
      request.method = String(frameDoc["method"] | "");
      request.path = String(frameDoc["path"] | "");
      if (!frameDoc["body"].isNull()) serializeJson(frameDoc["body"], request.body);
      response = api::handle(request);
    }

    JsonDocument responseDoc;
    responseDoc["type"] = "http_response";
    responseDoc["req_id"] = requestId;
    responseDoc["status"] = response.status;
    JsonDocument bodyDoc;
    if (!deserializeJson(bodyDoc, response.body)) {
      responseDoc["body"] = bodyDoc.as<JsonVariantConst>();
    } else {
      responseDoc["body"] = response.body;
    }
    String* reply = new String();
    if (reply != nullptr) {
      serializeJson(responseDoc, *reply);
      if (xQueueSend(s_responseQueue, &reply, pdMS_TO_TICKS(2000)) != pdTRUE) {
        delete reply;
      }
    }
    delete frame;
  }
}

bool parseUrl(const String& url, RelayEndpoint* endpoint) {
  String remaining = url;
  remaining.trim();
  bool tls = true;
  if (remaining.startsWith("wss://")) {
    tls = true;
    remaining = remaining.substring(6);
  } else if (remaining.startsWith("ws://")) {
    tls = false;
    remaining = remaining.substring(5);
  } else {
    return false;
  }
  if (remaining.isEmpty()) return false;

  int slash = remaining.indexOf('/');
  String hostPort = (slash < 0) ? remaining : remaining.substring(0, slash);
  String path = (slash < 0) ? String("/") : remaining.substring(slash);
  int colon = hostPort.indexOf(':');
  String host = hostPort;
  uint16_t port = tls ? 443 : 80;
  if (colon >= 0) {
    host = hostPort.substring(0, colon);
    const int parsedPort = hostPort.substring(colon + 1).toInt();
    if (parsedPort > 0 && parsedPort <= 65535) port = static_cast<uint16_t>(parsedPort);
  }
  if (host.isEmpty()) return false;

  endpoint->configured = true;
  endpoint->tls = tls;
  endpoint->host = host;
  endpoint->port = port;
  endpoint->path = path;
  return true;
}

}  // namespace

bool begin() {
  if (s_started) return true;

  const String url = String(RELAY_WS_URL);
  if (url.isEmpty()) {
    Serial.println("[relay] disabled (RELAY_WS_URL is empty)");
    appstate::setRelayStatus(false, false);
    return false;
  }
  if (strlen(DEVICE_TOKEN) == 0) {
    Serial.println("[relay] disabled (DEVICE_TOKEN is empty)");
    appstate::setRelayStatus(false, false);
    return false;
  }
  if (!parseUrl(url, &s_endpoint)) {
    Serial.println("[relay] disabled (invalid RELAY_WS_URL)");
    appstate::setRelayStatus(false, false);
    return false;
  }

  s_requestQueue = xQueueCreate(6, sizeof(RelayFrame*));
  s_responseQueue = xQueueCreate(6, sizeof(String*));
  if (s_requestQueue == nullptr || s_responseQueue == nullptr) {
    Serial.println("[relay] could not allocate queues");
    appstate::setRelayStatus(false, false);
    return false;
  }
  if (xTaskCreate(workerTask, "m5ir_relay", 12288, nullptr, tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    Serial.println("[relay] could not start worker task");
    appstate::setRelayStatus(false, false);
    return false;
  }

  // The library appends its own CRLF + headers after this string, so it must
  // NOT end with CRLF (a trailing CRLF would terminate the HTTP header block
  // early and the following "User-Agent" line would be sent as a WebSocket
  // frame, making Cloudflare close the socket with 1002 protocol error).
  s_extraHeaders = String("Authorization: Bearer ") + DEVICE_TOKEN;
  s_ws.onEvent(onWebSocketEvent);
  s_ws.setExtraHeaders(s_extraHeaders.c_str());
  s_ws.setReconnectInterval(0);  // bespoke exponential backoff below
  appstate::setRelayStatus(true, false);

  s_started = true;
  s_reconnectPending = true;
  s_nextAttemptMs = millis();
  Serial.printf("[relay] enabled: %s://%s:%u%s\n", s_endpoint.tls ? "wss" : "ws",
                s_endpoint.host.c_str(), static_cast<unsigned>(s_endpoint.port),
                s_endpoint.path.c_str());
  return true;
}

bool enabled() { return s_started; }

bool connected() { return s_connected; }

void loop() {
  if (!s_started) return;
  s_ws.loop();

  const uint32_t now = millis();

  if (s_connected) {
    String* reply = nullptr;
    while (xQueueReceive(s_responseQueue, &reply, 0) == pdTRUE) {
      if (reply != nullptr) {
        if (s_ws.isConnected()) s_ws.sendTXT(*reply);
        delete reply;
      }
    }
    if (now - s_lastPingMs >= M5IR_RELAY_PING_INTERVAL_MS) {
      s_lastPingMs = now;
      s_ws.sendTXT("{\"type\":\"ping\"}");
    }
    if (now - s_lastPongMs > M5IR_RELAY_PONG_TIMEOUT_MS) {
      Serial.println("[relay] pong timeout, reconnecting");
      s_ws.disconnect();
      scheduleReconnect();
    }
    return;
  }

  // Drop queued responses while offline; the relay has already timed them out.
  String* stale = nullptr;
  while (xQueueReceive(s_responseQueue, &stale, 0) == pdTRUE) delete stale;

  if (s_attemptInFlight && (now - s_attemptStartedMs) > M5IR_RELAY_CONNECT_TIMEOUT_MS) {
    Serial.println("[relay] connect timeout");
    s_ws.disconnect();
    scheduleReconnect();
  }
  if (s_reconnectPending && now >= s_nextAttemptMs) {
    attemptConnect();
  }
}

}  // namespace relayws
