#include "api_handler.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <map>
#include <vector>

#include "config.h"
#include "ir_engine.h"
#include "state.h"
#include "storage.h"

namespace api {
namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
ApiResponse finish(int status, JsonDocument& doc) {
  ApiResponse response;
  response.status = status;
  doc["ok"] = (status >= 200 && status < 300);
  serializeJson(doc, response.body);
  return response;
}

ApiResponse errorResponse(int status, const char* code, const String& message) {
  JsonDocument doc;
  JsonObject error = doc["error"].to<JsonObject>();
  error["code"] = code;
  error["message"] = message;
  ApiResponse response = finish(status, doc);
  if (response.status >= 200 && response.status < 300) response.status = status;
  return response;
}

ApiResponse okResponse(JsonDocument& doc) { return finish(200, doc); }

ApiResponse createdResponse(JsonDocument& doc) { return finish(201, doc); }

bool validId(const String& id, size_t maxLength) {
  if (id.isEmpty() || id.length() > maxLength) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool validDeviceId(const String& id) { return validId(id, M5IR_MAX_ID_LEN); }
bool validActionName(const String& name) { return validId(name, M5IR_MAX_ACTION_LEN); }

const char* formatName(const IrSignal& signal) {
  return signal.format == IrSignal::Format::Protocol ? "protocol" : "raw";
}

void splitPath(const String& path, std::vector<String>* parts) {
  parts->clear();
  int start = 0;
  const int length = static_cast<int>(path.length());
  for (int i = 0; i <= length; ++i) {
    if (i == length || path[i] == '/') {
      if (i > start) parts->push_back(path.substring(start, i));
      start = i + 1;
    }
  }
}

// ---------------------------------------------------------------------------
// GET /api/health
// ---------------------------------------------------------------------------
const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:
      return "task_watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep_sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

ApiResponse handleHealth() {
  JsonDocument doc;
  doc["version"] = M5IR_VERSION;
  doc["state"] = appstate::stateName();
  doc["uptime_ms"] = appstate::uptimeMs();
  doc["auth_required"] = (strlen(M5IR_API_KEY) > 0);

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  wifi["connected"] = wifiConnected;
  wifi["ssid"] = WiFi.SSID();
  wifi["ip"] = WiFi.localIP().toString();
  wifi["rssi"] = wifiConnected ? WiFi.RSSI() : 0;
  wifi["hostname"] = String(M5IR_HOSTNAME) + ".local";

  size_t fsTotal = 0;
  size_t fsUsed = 0;
  storage::stats(&fsTotal, &fsUsed);
  JsonObject filesystem = doc["filesystem"].to<JsonObject>();
  filesystem["total_bytes"] = fsTotal;
  filesystem["used_bytes"] = fsUsed;
  filesystem["free_bytes"] = (fsTotal > fsUsed) ? (fsTotal - fsUsed) : 0;

  JsonObject ir = doc["ir"].to<JsonObject>();
  ir["hardware"] = irengine::hardwareReady();
  ir["backend"] = irengine::backendName();
  ir["tx_pin"] = M5IR_IR_TX_PIN;
  ir["rx_pin"] = M5IR_IR_RX_PIN;
  if (!irengine::hardwareReady()) ir["reason"] = irengine::unavailableReason();

  JsonObject relay = doc["relay"].to<JsonObject>();
  relay["enabled"] = appstate::relayEnabled();
  relay["connected"] = appstate::relayConnected();

  doc["devices"] = static_cast<unsigned>(appstate::deviceCount());
  doc["message"] = appstate::statusMessage();
  doc["learn_remaining_ms"] = appstate::learnRemainingMs();
  doc["reset_reason"] = resetReasonName(esp_reset_reason());
  doc["free_heap"] = static_cast<unsigned>(ESP.getFreeHeap());
  return okResponse(doc);
}

// ---------------------------------------------------------------------------
// GET /api/devices
// ---------------------------------------------------------------------------
ApiResponse handleListDevices() {
  JsonDocument doc;
  JsonObject devices = doc["devices"].to<JsonObject>();
  const auto entries = appstate::listDevices();
  for (const auto& entry : entries) {
    JsonObject device = devices[entry.first].to<JsonObject>();
    device["label"] = entry.second.label;
    JsonArray actions = device["actions"].to<JsonArray>();
    for (const String& action : entry.second.actions) actions.add(action);
  }
  return okResponse(doc);
}

// ---------------------------------------------------------------------------
// POST /api/ir/learn
// ---------------------------------------------------------------------------
ApiResponse handleLearn(JsonDocument& body) {
  uint32_t timeoutMs = M5IR_LEARN_TIMEOUT_DEFAULT_MS;
  if (!body.isNull() && !body["timeout_ms"].isNull()) {
    JsonVariant value = body["timeout_ms"];
    if (!value.is<long>() && !value.is<unsigned long>()) {
      return errorResponse(400, "INVALID_REQUEST", "timeout_ms must be an integer");
    }
    const long requested = value.as<long>();
    if (requested <= 0) {
      return errorResponse(400, "INVALID_REQUEST", "timeout_ms must be positive");
    }
    timeoutMs = static_cast<uint32_t>(requested);
  }
  if (timeoutMs < M5IR_LEARN_TIMEOUT_MIN_MS) timeoutMs = M5IR_LEARN_TIMEOUT_MIN_MS;
  if (timeoutMs > M5IR_LEARN_TIMEOUT_MAX_MS) timeoutMs = M5IR_LEARN_TIMEOUT_MAX_MS;  // spec 4.3

  if (!irengine::hardwareReady()) {
    return errorResponse(503, "IR_NOT_AVAILABLE", irengine::unavailableReason());
  }

  String busyCode;
  String busyMessage;
  if (!appstate::tryBeginLearn(timeoutMs, &busyCode, &busyMessage)) {
    return errorResponse(409, busyCode.c_str(), busyMessage);
  }
  appstate::setStatusMessage("learning IR signal");

  IrSignal signal;
  String captureErrorCode;
  String captureErrorMessage;
  const bool captured = irengine::capture(timeoutMs, &signal, &captureErrorCode, &captureErrorMessage);
  appstate::endLearn();

  if (!captured) {
    const int status = captureErrorCode == "LEARN_TIMEOUT" ? 408 : 500;
    appstate::setStatusMessage("IR learning failed: " + captureErrorMessage);
    return errorResponse(status, captureErrorCode.c_str(), captureErrorMessage);
  }

  const String captureId = appstate::storeCapture(signal);
  if (captureId.isEmpty()) {
    appstate::setStatusMessage("capture buffer full");
    return errorResponse(503, "CAPTURE_BUFFER_FULL", "could not store the captured signal");
  }
  appstate::setStatusMessage("learned capture " + captureId);

  JsonDocument doc;
  doc["capture_id"] = captureId;
  if (signal.format == IrSignal::Format::Protocol) {
    doc["protocol"] = signal.protocol;
    doc["bits"] = signal.bits;
  } else {
    doc["protocol"] = "UNKNOWN";
    doc["bits"] = 0;
  }
  doc["raw_length"] = signal.rawEntries;
  return okResponse(doc);
}

// ---------------------------------------------------------------------------
// POST /api/ir/test
// ---------------------------------------------------------------------------
ApiResponse handleTest(JsonDocument& body) {
  if (body.isNull() || !body["capture_id"].is<const char*>()) {
    return errorResponse(400, "INVALID_REQUEST", "capture_id is required");
  }
  const String captureId = body["capture_id"].as<const char*>();
  IrSignal signal;
  if (!appstate::getCapture(captureId, &signal)) {
    return errorResponse(404, "CAPTURE_NOT_FOUND", "unknown or expired capture_id: " + captureId);
  }
  if (!irengine::hardwareReady()) {
    return errorResponse(503, "IR_NOT_AVAILABLE", irengine::unavailableReason());
  }

  String busyCode;
  String busyMessage;
  if (!appstate::tryBeginSend(&busyCode, &busyMessage)) {
    return errorResponse(409, busyCode.c_str(), busyMessage);
  }
  String sendCode;
  String sendMessage;
  const bool sent = irengine::send(signal, &sendCode, &sendMessage);
  appstate::endSend();
  if (!sent) {
    return errorResponse(500, sendCode.c_str(), sendMessage);
  }

  JsonDocument doc;
  doc["sent"] = true;
  doc["capture_id"] = captureId;
  return okResponse(doc);
}

// ---------------------------------------------------------------------------
// POST /api/devices/{device_id}/actions
// ---------------------------------------------------------------------------
ApiResponse handleSaveAction(const String& deviceId, JsonDocument& body) {
  if (!validDeviceId(deviceId)) {
    return errorResponse(400, "INVALID_DEVICE_ID",
                         "device_id may only contain [a-z0-9_] (max 24 characters)");
  }
  if (body.isNull() || !body.is<JsonObject>()) {
    return errorResponse(400, "INVALID_REQUEST", "JSON object body is required");
  }
  if (!body["action"].is<const char*>() || !body["capture_id"].is<const char*>()) {
    return errorResponse(400, "INVALID_REQUEST", "action and capture_id are required");
  }
  const String action = body["action"].as<const char*>();
  const String captureId = body["capture_id"].as<const char*>();
  if (!validActionName(action)) {
    return errorResponse(400, "INVALID_ACTION",
                         "action may only contain [a-z0-9_] (max 32 characters)");
  }
  String label;
  if (!body["label"].isNull()) {
    if (!body["label"].is<const char*>()) {
      return errorResponse(400, "INVALID_REQUEST", "label must be a string");
    }
    label = body["label"].as<const char*>();
    if (label.length() > M5IR_MAX_LABEL_LEN) {
      return errorResponse(400, "INVALID_REQUEST", "label is too long (max 64 bytes)");
    }
  }

  DeviceSummary existing;
  const bool deviceExists = appstate::getDeviceSummary(deviceId, &existing);
  if (!deviceExists && label.isEmpty()) {
    return errorResponse(400, "INVALID_REQUEST", "label is required when creating a new device");
  }

  IrSignal signal;
  if (!appstate::getCapture(captureId, &signal)) {
    return errorResponse(404, "CAPTURE_NOT_FOUND", "unknown or expired capture_id: " + captureId);
  }

  String error;
  bool createdDevice = false;
  if (!appstate::upsertAction(deviceId, label, action, signal, &createdDevice, &error)) {
    return errorResponse(500, "STORAGE_ERROR", error);
  }
  appstate::setStatusMessage("stored " + deviceId + "/" + action);

  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["action"] = action;
  doc["stored_format"] = formatName(signal);
  return createdResponse(doc);
}

// ---------------------------------------------------------------------------
// POST /api/ir/send
// ---------------------------------------------------------------------------
struct SendStep {
  bool isDelay = false;
  uint32_t delayMs = 0;
  String deviceId;
  String action;
  IrAction resolved;
};

ApiResponse handleSend(JsonDocument& body) {
  if (body.isNull()) {
    return errorResponse(400, "INVALID_REQUEST", "JSON body is required");
  }

  std::vector<SendStep> steps;
  if (!body["steps"].isNull()) {
    if (!body["steps"].is<JsonArray>()) {
      return errorResponse(400, "INVALID_REQUEST", "steps must be an array");
    }
    JsonArray array = body["steps"].as<JsonArray>();
    if (array.size() == 0) {
      return errorResponse(400, "INVALID_REQUEST", "steps must not be empty");
    }
    if (array.size() > M5IR_SEND_MAX_STEPS) {
      return errorResponse(400, "STEPS_LIMIT_EXCEEDED",
                           String("at most ") + M5IR_SEND_MAX_STEPS + " steps are allowed");
    }
    steps.reserve(array.size());
    for (JsonVariant item : array) {
      if (!item.is<JsonObject>()) {
        return errorResponse(400, "INVALID_REQUEST", "each step must be an object");
      }
      JsonObject object = item.as<JsonObject>();
      SendStep step;
      if (!object["delay_ms"].isNull()) {
        if (!object["delay_ms"].is<long>() && !object["delay_ms"].is<unsigned long>()) {
          return errorResponse(400, "INVALID_REQUEST", "delay_ms must be an integer");
        }
        const long delayMs = object["delay_ms"].as<long>();
        if (delayMs < 0 || static_cast<uint32_t>(delayMs) > M5IR_SEND_MAX_DELAY_MS) {
          return errorResponse(400, "INVALID_REQUEST",
                               "delay_ms must be between 0 and 5000 (spec 4.3)");
        }
        step.isDelay = true;
        step.delayMs = static_cast<uint32_t>(delayMs);
      } else {
        if (!object["device"].is<const char*>() || !object["action"].is<const char*>()) {
          return errorResponse(400, "INVALID_REQUEST", "each step needs device and action");
        }
        step.deviceId = object["device"].as<const char*>();
        step.action = object["action"].as<const char*>();
        if (!validDeviceId(step.deviceId) || !validActionName(step.action)) {
          return errorResponse(400, "INVALID_REQUEST", "invalid device or action name");
        }
      }
      steps.push_back(step);
    }
  } else {
    if (!body["device"].is<const char*>() || !body["action"].is<const char*>()) {
      return errorResponse(400, "INVALID_REQUEST",
                           "provide either device/action or a steps array");
    }
    SendStep step;
    step.deviceId = body["device"].as<const char*>();
    step.action = body["action"].as<const char*>();
    if (!validDeviceId(step.deviceId) || !validActionName(step.action)) {
      return errorResponse(400, "INVALID_REQUEST", "invalid device or action name");
    }
    steps.push_back(step);
  }

  if (!irengine::hardwareReady()) {
    return errorResponse(503, "IR_NOT_AVAILABLE", irengine::unavailableReason());
  }

  // Resolve every non-delay step before transmitting anything, so a typo does
  // not leave the user with a half-executed scene.
  for (SendStep& step : steps) {
    if (step.isDelay) continue;
    DeviceSummary device;
    if (!appstate::getDeviceSummary(step.deviceId, &device)) {
      return errorResponse(404, "DEVICE_NOT_FOUND", "no such device: " + step.deviceId);
    }
    if (!appstate::getAction(step.deviceId, step.action, &step.resolved)) {
      return errorResponse(404, "ACTION_NOT_FOUND",
                           "no such action: " + step.deviceId + "/" + step.action);
    }
  }

  String busyCode;
  String busyMessage;
  if (!appstate::tryBeginSend(&busyCode, &busyMessage)) {
    return errorResponse(409, busyCode.c_str(), busyMessage);
  }

  unsigned executed = 0;
  for (const SendStep& step : steps) {
    if (step.isDelay) {
      if (step.delayMs > 0) vTaskDelay(pdMS_TO_TICKS(step.delayMs));
      ++executed;
      continue;
    }
    appstate::setStatusMessage("sending " + step.deviceId + "/" + step.action);
    String sendCode;
    String sendMessage;
    if (!irengine::send(step.resolved.signal, &sendCode, &sendMessage)) {
      appstate::endSend();
      JsonDocument doc;
      doc["executed_steps"] = executed;
      JsonObject error = doc["error"].to<JsonObject>();
      error["code"] = sendCode;
      error["message"] = sendMessage;
      ApiResponse response = finish(500, doc);
      response.status = 500;
      return response;
    }
    ++executed;
  }
  appstate::endSend();
  appstate::setStatusMessage("sent " + String(executed) + " step(s)");

  JsonDocument doc;
  doc["executed_steps"] = executed;
  return okResponse(doc);
}

// ---------------------------------------------------------------------------
// DELETE /api/devices/{device_id}
// ---------------------------------------------------------------------------
ApiResponse handleDeleteDevice(const String& deviceId) {
  if (!validDeviceId(deviceId)) {
    return errorResponse(400, "INVALID_DEVICE_ID", "invalid device_id");
  }
  DeviceSummary existing;
  if (!appstate::getDeviceSummary(deviceId, &existing)) {
    return errorResponse(404, "DEVICE_NOT_FOUND", "no such device: " + deviceId);
  }
  String error;
  if (!appstate::deleteDevice(deviceId, &error)) {
    return errorResponse(500, "STORAGE_ERROR", error);
  }
  appstate::setStatusMessage("deleted device " + deviceId);
  JsonDocument doc;
  doc["device_id"] = deviceId;
  return okResponse(doc);
}

// ---------------------------------------------------------------------------
// DELETE /api/devices/{device_id}/actions/{action}
// ---------------------------------------------------------------------------
ApiResponse handleDeleteAction(const String& deviceId, const String& action) {
  if (!validDeviceId(deviceId) || !validActionName(action)) {
    return errorResponse(400, "INVALID_REQUEST", "invalid device_id or action");
  }
  DeviceSummary existing;
  if (!appstate::getDeviceSummary(deviceId, &existing)) {
    return errorResponse(404, "DEVICE_NOT_FOUND", "no such device: " + deviceId);
  }
  bool found = false;
  for (const String& name : existing.actions) {
    if (name == action) {
      found = true;
      break;
    }
  }
  if (!found) {
    return errorResponse(404, "ACTION_NOT_FOUND",
                         "no such action: " + deviceId + "/" + action);
  }
  String error;
  if (!appstate::deleteAction(deviceId, action, &error)) {
    return errorResponse(500, "STORAGE_ERROR", error);
  }
  appstate::setStatusMessage("deleted action " + deviceId + "/" + action);
  JsonDocument doc;
  doc["device_id"] = deviceId;
  doc["action"] = action;
  return okResponse(doc);
}

}  // namespace

const char* statusText(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 409:
      return "Conflict";
    case 413:
      return "Payload Too Large";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Timeout";
    default:
      return "Unknown";
  }
}

ApiResponse handle(const ApiRequest& request) {
  std::vector<String> parts;
  splitPath(request.path, &parts);
  const String method = request.method;
  auto methodNotAllowed = [&]() {
    return errorResponse(405, "METHOD_NOT_ALLOWED",
                         method + " is not supported for " + request.path);
  };

  // Parse the JSON body once; GET/DELETE may have an empty body.
  JsonDocument body;
  if (!request.body.isEmpty()) {
    const DeserializationError error = deserializeJson(body, request.body);
    if (error) {
      return errorResponse(400, "INVALID_JSON", error.c_str());
    }
  }

  if (parts.size() < 2 || parts[0] != "api") {
    return errorResponse(404, "NOT_FOUND", "unknown endpoint: " + request.path);
  }

  // GET /api/health
  if (parts.size() == 2 && parts[1] == "health") {
    if (method != "GET") return methodNotAllowed();
    return handleHealth();
  }

  // GET /api/diag/last_capture (authenticated diagnostics)
  if (parts.size() == 3 && parts[1] == "diag" && parts[2] == "last_capture") {
    if (method != "GET") return methodNotAllowed();
    String captureId;
    IrSignal signal;
    if (!appstate::getLatestCapture(&captureId, &signal)) {
      return errorResponse(404, "CAPTURE_NOT_FOUND", "no capture is currently buffered");
    }
    JsonDocument doc;
    doc["capture_id"] = captureId;
    doc["format"] = (signal.format == IrSignal::Format::Protocol) ? "protocol" : "raw";
    doc["protocol"] = signal.protocol.isEmpty() ? "UNKNOWN" : signal.protocol;
    doc["bits"] = signal.bits;
    doc["freq_khz"] = signal.freqKhz;
    doc["raw_length"] = signal.rawEntries;
    if (!signal.state.empty()) {
      JsonArray stateArray = doc["state"].to<JsonArray>();
      for (const uint8_t value : signal.state) stateArray.add(value);
    }
    JsonArray head = doc["raw_head"].to<JsonArray>();
    const size_t limit = signal.raw.size() < 16 ? signal.raw.size() : 16;
    for (size_t i = 0; i < limit; ++i) head.add(signal.raw[i]);
    return okResponse(doc);
  }

  // GET /api/devices
  if (parts.size() == 2 && parts[1] == "devices") {
    if (method != "GET") return methodNotAllowed();
    return handleListDevices();
  }

  // /api/ir/*
  if (parts.size() == 3 && parts[1] == "ir") {
    if (method != "POST") return methodNotAllowed();
    if (parts[2] == "learn") return handleLearn(body);
    if (parts[2] == "test") return handleTest(body);
    if (parts[2] == "send") return handleSend(body);
    return errorResponse(404, "NOT_FOUND", "unknown endpoint: " + request.path);
  }

  // /api/devices/{device_id}/actions
  if (parts.size() == 4 && parts[1] == "devices" && parts[3] == "actions") {
    if (method != "POST") return methodNotAllowed();
    return handleSaveAction(parts[2], body);
  }

  // /api/devices/{device_id}/actions/{action}
  if (parts.size() == 5 && parts[1] == "devices" && parts[3] == "actions") {
    if (method != "DELETE") return methodNotAllowed();
    return handleDeleteAction(parts[2], parts[4]);
  }

  // /api/devices/{device_id}
  if (parts.size() == 3 && parts[1] == "devices") {
    if (method != "DELETE") return methodNotAllowed();
    return handleDeleteDevice(parts[2]);
  }

  return errorResponse(404, "NOT_FOUND", "unknown endpoint: " + request.path);
}

}  // namespace api
