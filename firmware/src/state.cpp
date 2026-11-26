#include "state.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage.h"

namespace {

SemaphoreHandle_t s_mutex = nullptr;

RunState s_state = RunState::Boot;
String s_statusMessage;
uint32_t s_stateSinceMs = 0;
uint32_t s_bootMs = 0;

uint32_t s_learnTimeoutMs = 0;
uint32_t s_learnDeadlineMs = 0;

std::map<String, IrDevice> s_devices;

bool s_relayEnabled = false;
bool s_relayConnected = false;

struct CaptureSlot {
  bool valid = false;
  String id;
  IrSignal signal;
  uint32_t storedAtMs = 0;
};

CaptureSlot s_captures[M5IR_CAPTURE_SLOTS];
uint8_t s_captureCursor = 0;

// RAII mutex guard for the state lock. Every public appstate function takes
// this lock exactly once; private helpers assume it is already held.
class StateLock {
 public:
  StateLock() {
    if (s_mutex != nullptr) xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  ~StateLock() {
    if (s_mutex != nullptr) xSemaphoreGive(s_mutex);
  }
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;
};

bool captureExpired(const CaptureSlot& slot, uint32_t now) {
  return slot.valid && (now - slot.storedAtMs) >= M5IR_CAPTURE_TTL_MS;
}

// Persists s_devices. Caller must hold the state lock.
bool persistLocked(String* errMsg) {
  return storage::save(s_devices, errMsg);
}

}  // namespace

const char* runStateName(RunState state) {
  switch (state) {
    case RunState::Boot:
      return "boot";
    case RunState::Connecting:
      return "connecting";
    case RunState::Idle:
      return "idle";
    case RunState::Learning:
      return "learning";
    case RunState::Sending:
      return "sending";
    case RunState::Error:
      return "error";
  }
  return "unknown";
}

namespace appstate {
namespace {

// Generates a short unique capture id such as "cap_8f2a".
String makeCaptureIdLocked() {
  for (int attempt = 0; attempt < 16; ++attempt) {
    char buffer[12];
    snprintf(buffer, sizeof(buffer), "cap_%04x", static_cast<unsigned>(esp_random() & 0xFFFFUL));
    bool duplicate = false;
    for (const CaptureSlot& slot : s_captures) {
      if (slot.valid && slot.id == buffer) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) return String(buffer);
  }
  return String();
}

}  // namespace

void begin() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
  s_bootMs = millis();
  {
    StateLock lock;
    s_state = RunState::Boot;
    s_statusMessage = "booting";
    s_stateSinceMs = s_bootMs;
  }
}

void tick() {
  cleanupCaptures();
}

void cleanupCaptures() {
  StateLock lock;
  const uint32_t now = millis();
  for (CaptureSlot& slot : s_captures) {
    if (captureExpired(slot, now)) {
      slot.valid = false;
      slot.id = "";
      slot.signal = IrSignal();
    }
  }
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
RunState state() {
  StateLock lock;
  return s_state;
}

const char* stateName() {
  return runStateName(state());
}

void setState(RunState newState) {
  StateLock lock;
  if (s_state != newState) {
    s_state = newState;
    s_stateSinceMs = millis();
  }
}

String statusMessage() {
  StateLock lock;
  return s_statusMessage;
}

void setStatusMessage(const String& message) {
  StateLock lock;
  s_statusMessage = message;
}

uint32_t uptimeMs() {
  return millis() - s_bootMs;
}

bool tryBeginLearn(uint32_t timeoutMs, String* errCode, String* errMsg) {
  StateLock lock;
  if (s_state == RunState::Learning) {
    if (errCode != nullptr) *errCode = "IR_BUSY";
    if (errMsg != nullptr) *errMsg = "an IR learning session is already in progress";
    return false;
  }
  if (s_state == RunState::Sending) {
    if (errCode != nullptr) *errCode = "IR_BUSY";
    if (errMsg != nullptr) *errMsg = "an IR transmission is in progress";
    return false;
  }
  s_state = RunState::Learning;
  s_stateSinceMs = millis();
  s_learnTimeoutMs = timeoutMs;
  s_learnDeadlineMs = millis() + timeoutMs;
  if (errCode != nullptr) errCode->clear();
  if (errMsg != nullptr) errMsg->clear();
  return true;
}

void endLearn() {
  StateLock lock;
  if (s_state == RunState::Learning) {
    s_state = RunState::Idle;
    s_stateSinceMs = millis();
  }
}

bool tryBeginSend(String* errCode, String* errMsg) {
  StateLock lock;
  if (s_state == RunState::Learning) {
    if (errCode != nullptr) *errCode = "IR_BUSY";
    if (errMsg != nullptr) *errMsg = "an IR learning session is in progress";
    return false;
  }
  if (s_state == RunState::Sending) {
    if (errCode != nullptr) *errCode = "IR_BUSY";
    if (errMsg != nullptr) *errMsg = "another IR transmission is in progress";
    return false;
  }
  s_state = RunState::Sending;
  s_stateSinceMs = millis();
  if (errCode != nullptr) errCode->clear();
  if (errMsg != nullptr) errMsg->clear();
  return true;
}

void endSend() {
  StateLock lock;
  if (s_state == RunState::Sending) {
    s_state = RunState::Idle;
    s_stateSinceMs = millis();
  }
}

uint32_t learnRemainingMs() {
  StateLock lock;
  if (s_state != RunState::Learning) return 0;
  const uint32_t now = millis();
  return (now >= s_learnDeadlineMs) ? 0 : (s_learnDeadlineMs - now);
}

uint32_t learnTimeoutMs() {
  StateLock lock;
  return s_learnTimeoutMs;
}

// ---------------------------------------------------------------------------
// Device database
// ---------------------------------------------------------------------------
size_t deviceCount() {
  StateLock lock;
  return s_devices.size();
}

std::vector<std::pair<String, DeviceSummary>> listDevices() {
  StateLock lock;
  std::vector<std::pair<String, DeviceSummary>> result;
  result.reserve(s_devices.size());
  for (const auto& entry : s_devices) {
    DeviceSummary summary;
    summary.label = entry.second.label;
    summary.actions.reserve(entry.second.actions.size());
    for (const auto& action : entry.second.actions) {
      summary.actions.push_back(action.first);
    }
    result.emplace_back(entry.first, summary);
  }
  return result;
}

bool getDeviceSummary(const String& deviceId, DeviceSummary* out) {
  StateLock lock;
  const auto it = s_devices.find(deviceId);
  if (it == s_devices.end()) return false;
  out->label = it->second.label;
  out->actions.clear();
  out->actions.reserve(it->second.actions.size());
  for (const auto& action : it->second.actions) out->actions.push_back(action.first);
  return true;
}

bool getAction(const String& deviceId, const String& action, IrAction* out) {
  StateLock lock;
  const auto deviceIt = s_devices.find(deviceId);
  if (deviceIt == s_devices.end()) return false;
  const auto actionIt = deviceIt->second.actions.find(action);
  if (actionIt == deviceIt->second.actions.end()) return false;
  *out = actionIt->second;
  return true;
}

bool upsertAction(const String& deviceId, const String& label, const String& action,
                  const IrSignal& signal, bool* createdDevice, String* errMsg) {
  if (createdDevice != nullptr) *createdDevice = false;

  StateLock lock;
  auto deviceIt = s_devices.find(deviceId);
  const bool isNewDevice = (deviceIt == s_devices.end());
  if (isNewDevice) {
    if (s_devices.size() >= M5IR_MAX_DEVICES) {
      if (errMsg != nullptr) *errMsg = "device limit reached";
      return false;
    }
    IrDevice device;
    device.label = label;
    const auto insertResult = s_devices.emplace(deviceId, device);
    if (!insertResult.second) {
      if (errMsg != nullptr) *errMsg = "failed to allocate device";
      return false;
    }
    deviceIt = insertResult.first;
    if (createdDevice != nullptr) *createdDevice = true;
  } else if (!label.isEmpty()) {
    deviceIt->second.label = label;
  }

  if (deviceIt->second.actions.size() >= M5IR_MAX_ACTIONS_PER_DEVICE &&
      deviceIt->second.actions.find(action) == deviceIt->second.actions.end()) {
    if (errMsg != nullptr) *errMsg = "action limit reached for this device";
    return false;
  }
  deviceIt->second.actions[action].signal = signal;

  String saveError;
  if (!persistLocked(&saveError)) {
    if (errMsg != nullptr) *errMsg = "failed to persist database: " + saveError;
    return false;
  }
  return true;
}

bool deleteDevice(const String& deviceId, String* errMsg) {
  StateLock lock;
  const auto it = s_devices.find(deviceId);
  if (it == s_devices.end()) {
    if (errMsg != nullptr) *errMsg = "device not found";
    return false;
  }
  s_devices.erase(it);
  String saveError;
  if (!persistLocked(&saveError)) {
    if (errMsg != nullptr) *errMsg = "failed to persist database: " + saveError;
    return false;
  }
  return true;
}

bool deleteAction(const String& deviceId, const String& action, String* errMsg) {
  StateLock lock;
  const auto deviceIt = s_devices.find(deviceId);
  if (deviceIt == s_devices.end()) {
    if (errMsg != nullptr) *errMsg = "device not found";
    return false;
  }
  const auto actionIt = deviceIt->second.actions.find(action);
  if (actionIt == deviceIt->second.actions.end()) {
    if (errMsg != nullptr) *errMsg = "action not found";
    return false;
  }
  deviceIt->second.actions.erase(actionIt);
  String saveError;
  if (!persistLocked(&saveError)) {
    if (errMsg != nullptr) *errMsg = "failed to persist database: " + saveError;
    return false;
  }
  return true;
}

bool loadDatabase() {
  String error;
  std::map<String, IrDevice> loaded;
  const bool ok = storage::load(&loaded, &error);
  StateLock lock;
  s_devices.swap(loaded);
  if (!ok && !error.isEmpty()) {
    s_statusMessage = "database load failed: " + error;
  }
  return ok;
}

bool saveDatabase() {
  String error;
  StateLock lock;
  return persistLocked(&error);
}

// ---------------------------------------------------------------------------
// Temporary capture buffer
// ---------------------------------------------------------------------------
String storeCapture(const IrSignal& signal) {
  StateLock lock;
  const uint32_t now = millis();

  int slotIndex = -1;
  for (int i = 0; i < M5IR_CAPTURE_SLOTS; ++i) {
    if (!s_captures[i].valid || captureExpired(s_captures[i], now)) {
      slotIndex = i;
      break;
    }
  }
  if (slotIndex < 0) {
    slotIndex = s_captureCursor % M5IR_CAPTURE_SLOTS;
    s_captureCursor = static_cast<uint8_t>((s_captureCursor + 1) % M5IR_CAPTURE_SLOTS);
  }

  CaptureSlot& slot = s_captures[slotIndex];
  slot.valid = false;
  slot.id = makeCaptureIdLocked();
  if (slot.id.isEmpty()) return String();
  slot.signal = signal;
  slot.storedAtMs = now;
  slot.valid = true;
  return slot.id;
}

bool getCapture(const String& captureId, IrSignal* out) {
  StateLock lock;
  const uint32_t now = millis();
  for (CaptureSlot& slot : s_captures) {
    if (!slot.valid || slot.id != captureId) continue;
    if (captureExpired(slot, now)) {
      slot.valid = false;
      slot.id = "";
      return false;
    }
    *out = slot.signal;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Relay status
// ---------------------------------------------------------------------------
void setRelayStatus(bool enabled, bool connected) {
  StateLock lock;
  s_relayEnabled = enabled;
  s_relayConnected = connected;
}

bool relayEnabled() {
  StateLock lock;
  return s_relayEnabled;
}

bool relayConnected() {
  StateLock lock;
  return s_relayConnected;
}

}  // namespace appstate
