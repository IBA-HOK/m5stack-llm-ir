#include "storage.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

namespace storage {
namespace {

constexpr const char* kCorruptPath = "/ir_db.corrupt.json";

bool validId(const String& id) {
  if (id.isEmpty() || id.length() > M5IR_MAX_ID_LEN) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool validActionName(const String& name) {
  if (name.isEmpty() || name.length() > M5IR_MAX_ACTION_LEN) return false;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

// Parses "0x20DF10EF" / "20DF10EF" / decimal strings into a 64-bit code.
bool parseCode(const char* text, uint64_t* out) {
  if (text == nullptr || *text == '\0') return false;
  char* end = nullptr;
  errno = 0;
  const uint64_t value = strtoull(text, &end, 0);  // base 0 handles 0x prefix
  if (errno != 0 || end == text || *end != '\0') return false;
  *out = value;
  return true;
}

void clearDeviceMap(std::map<String, IrDevice>* devices) {
  devices->clear();
}

bool loadDeviceAction(const JsonObject& actionObj, IrAction* out) {
  if (actionObj.isNull()) return false;
  const char* format = actionObj["format"] | "";
  IrSignal& signal = out->signal;
  signal.raw.clear();

  if (strcmp(format, "protocol") == 0) {
    const char* protocol = actionObj["protocol"] | "";
    const char* codeText = actionObj["code"] | "";
    const uint16_t bits = actionObj["bits"] | 0;
    if (protocol[0] == '\0' || strlen(protocol) >= M5IR_MAX_PROTOCOL_LEN) return false;
    if (bits == 0 || bits > 64) return false;
    uint64_t code = 0;
    if (!parseCode(codeText, &code)) return false;
    signal.format = IrSignal::Format::Protocol;
    signal.protocol = protocol;
    signal.code = code;
    signal.bits = bits;
    signal.freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
    return true;
  }

  if (strcmp(format, "raw") == 0) {
    JsonArray raw = actionObj["raw_data"].as<JsonArray>();
    if (raw.isNull() || raw.size() == 0 || raw.size() > M5IR_MAX_RAW_LEN) return false;
    const uint16_t freq = actionObj["freq_khz"] | M5IR_IR_DEFAULT_FREQ_KHZ;
    signal.format = IrSignal::Format::Raw;
    signal.protocol = "";
    signal.code = 0;
    signal.bits = 0;
    signal.freqKhz = (freq >= 20 && freq <= 60) ? freq : M5IR_IR_DEFAULT_FREQ_KHZ;
    signal.raw.reserve(raw.size());
    for (JsonVariant value : raw) {
      const uint32_t micros = value.as<uint32_t>();
      if (micros == 0 || micros > 65535UL) return false;
      signal.raw.push_back(static_cast<uint16_t>(micros));
    }
    return true;
  }

  return false;
}

}  // namespace

bool begin() {
  if (!LittleFS.begin(true)) {
    Serial.println("[storage] LittleFS mount failed");
    return false;
  }
  Serial.printf("[storage] LittleFS mounted: %u/%u bytes used\n",
                static_cast<unsigned>(LittleFS.usedBytes()),
                static_cast<unsigned>(LittleFS.totalBytes()));
  return true;
}

bool load(std::map<String, IrDevice>* devices, String* err) {
  clearDeviceMap(devices);
  if (!LittleFS.exists(M5IR_DB_PATH)) {
    Serial.println("[storage] no database yet, starting empty");
    return true;
  }

  File file = LittleFS.open(M5IR_DB_PATH, "r");
  if (!file) {
    if (err != nullptr) *err = "cannot open /ir_db.json";
    return false;
  }
  if (file.size() > M5IR_DB_MAX_BYTES) {
    file.close();
    if (err != nullptr) *err = "database file is too large";
    return false;
  }

  JsonDocument doc;
  const DeserializationError parseError = deserializeJson(doc, file);
  file.close();
  if (parseError) {
    Serial.printf("[storage] corrupt database: %s\n", parseError.c_str());
    LittleFS.remove(kCorruptPath);
    LittleFS.rename(M5IR_DB_PATH, kCorruptPath);
    if (err != nullptr) *err = String("corrupt database: ") + parseError.c_str();
    return false;
  }

  JsonObject devicesObj = doc["devices"].as<JsonObject>();
  if (devicesObj.isNull()) {
    Serial.println("[storage] database has no devices object, starting empty");
    return true;
  }

  for (JsonPair devicePair : devicesObj) {
    const String deviceId = devicePair.key().c_str();
    if (!validId(deviceId)) continue;
    if (devices->size() >= M5IR_MAX_DEVICES) break;

    JsonObject deviceObj = devicePair.value().as<JsonObject>();
    if (deviceObj.isNull()) continue;

    IrDevice device;
    device.label = String(deviceObj["label"] | deviceId.c_str());
    if (device.label.isEmpty()) device.label = deviceId;
    if (device.label.length() > M5IR_MAX_LABEL_LEN) device.label = device.label.substring(0, M5IR_MAX_LABEL_LEN);

    JsonObject actionsObj = deviceObj["actions"].as<JsonObject>();
    if (!actionsObj.isNull()) {
      for (JsonPair actionPair : actionsObj) {
        if (device.actions.size() >= M5IR_MAX_ACTIONS_PER_DEVICE) break;
        const String actionName = actionPair.key().c_str();
        if (!validActionName(actionName)) continue;
        IrAction action;
        if (!loadDeviceAction(actionPair.value().as<JsonObject>(), &action)) {
          Serial.printf("[storage] skipping invalid action %s/%s\n", deviceId.c_str(), actionName.c_str());
          continue;
        }
        device.actions[actionName] = action;
      }
    }
    (*devices)[deviceId] = device;
  }

  Serial.printf("[storage] loaded %u devices\n", static_cast<unsigned>(devices->size()));
  return true;
}

bool save(const std::map<String, IrDevice>& devices, String* err) {
  JsonDocument doc;
  doc["schema_version"] = M5IR_DB_SCHEMA_VERSION;
  JsonObject devicesObj = doc["devices"].to<JsonObject>();

  for (const auto& devicePair : devices) {
    JsonObject deviceObj = devicesObj[devicePair.first].to<JsonObject>();
    deviceObj["label"] = devicePair.second.label;
    JsonObject actionsObj = deviceObj["actions"].to<JsonObject>();
    for (const auto& actionPair : devicePair.second.actions) {
      JsonObject actionObj = actionsObj[actionPair.first].to<JsonObject>();
      const IrSignal& signal = actionPair.second.signal;
      if (signal.format == IrSignal::Format::Protocol) {
        actionObj["format"] = "protocol";
        actionObj["protocol"] = signal.protocol;
        char hex[24];
        snprintf(hex, sizeof(hex), "0x%llX", static_cast<unsigned long long>(signal.code));
        actionObj["code"] = hex;
        actionObj["bits"] = signal.bits;
      } else {
        actionObj["format"] = "raw";
        actionObj["freq_khz"] = signal.freqKhz;
        JsonArray raw = actionObj["raw_data"].to<JsonArray>();
        for (const uint16_t value : signal.raw) raw.add(value);
      }
    }
  }

  LittleFS.remove(M5IR_DB_TMP_PATH);
  File file = LittleFS.open(M5IR_DB_TMP_PATH, "w");
  if (!file) {
    if (err != nullptr) *err = "cannot open /ir_db.tmp for writing";
    return false;
  }
  const size_t written = serializeJson(doc, file);
  file.flush();
  file.close();
  if (written == 0) {
    LittleFS.remove(M5IR_DB_TMP_PATH);
    if (err != nullptr) *err = "failed to serialize database";
    return false;
  }

  // Rename-based atomic replacement with rollback (spec 7.2).
  if (LittleFS.exists(M5IR_DB_PATH)) {
    LittleFS.remove(M5IR_DB_BAK_PATH);
    if (!LittleFS.rename(M5IR_DB_PATH, M5IR_DB_BAK_PATH)) {
      // Some LittleFS mounts allow overwriting renames; try removing instead.
      LittleFS.remove(M5IR_DB_PATH);
    }
  }
  if (!LittleFS.rename(M5IR_DB_TMP_PATH, M5IR_DB_PATH)) {
    if (LittleFS.exists(M5IR_DB_BAK_PATH)) {
      LittleFS.rename(M5IR_DB_BAK_PATH, M5IR_DB_PATH);
    }
    LittleFS.remove(M5IR_DB_TMP_PATH);
    if (err != nullptr) *err = "failed to replace /ir_db.json";
    return false;
  }
  if (LittleFS.exists(M5IR_DB_BAK_PATH)) LittleFS.remove(M5IR_DB_BAK_PATH);
  Serial.printf("[storage] saved %u devices (%u bytes)\n",
                static_cast<unsigned>(devices.size()), static_cast<unsigned>(written));
  return true;
}

void stats(size_t* totalBytes, size_t* usedBytes) {
  if (totalBytes != nullptr) *totalBytes = LittleFS.totalBytes();
  if (usedBytes != nullptr) *usedBytes = LittleFS.usedBytes();
}

}  // namespace storage
