// Shared runtime state: device database, capture buffers, state machine.
//
// This module is the single owner of the mutable state that the HTTP API and
// the relay WebSocket client share. All public functions are thread-safe
// (they take an internal mutex) and must not be called while the caller holds
// that mutex.
#pragma once

#include <Arduino.h>

#include <map>
#include <utility>
#include <vector>

#include "config.h"

enum class RunState : uint8_t { Boot, Connecting, Idle, Learning, Sending, Error };

// Lower-case state name used by GET /api/health ("idle", "learning", ...).
const char* runStateName(RunState state);

// A learned infrared signal. Known protocols are stored compactly; unknown
// protocols fall back to the raw microsecond timing array (spec 7.1).
struct IrSignal {
  enum class Format : uint8_t { Protocol = 0, Raw = 1 };

  Format format = Format::Protocol;
  String protocol;                    // "NEC", "SONY", ... (protocol format only)
  uint64_t code = 0;                  // protocol format only
  uint16_t bits = 0;                  // protocol format only
  uint16_t freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;  // raw format only
  std::vector<uint16_t> raw;          // raw format only (microseconds)
  // Number of raw timing entries seen during the capture. Reported by
  // POST /api/ir/learn as `raw_length`; not persisted.
  uint16_t rawEntries = 0;

  bool empty() const {
    return format == Format::Protocol ? protocol.isEmpty() : raw.empty();
  }
  size_t rawLength() const { return raw.size(); }
};

struct IrAction {
  IrSignal signal;
};

struct IrDevice {
  String label;
  std::map<String, IrAction> actions;
};

// Light-weight view returned by GET /api/devices.
struct DeviceSummary {
  String label;
  std::vector<String> actions;
};

namespace appstate {

// Initialises the mutex and loads the database from LittleFS. Call once from
// setup() after storage::begin().
void begin();

// Periodic housekeeping (expired capture buffers). Call from loop().
void tick();

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
RunState state();
const char* stateName();
void setState(RunState newState);
String statusMessage();
void setStatusMessage(const String& message);
uint32_t uptimeMs();

// LEARNING / SENDING coordination. tryBegin* returns false and fills errCode /
// errMsg when another IR operation is in progress (409 Conflict).
bool tryBeginLearn(uint32_t timeoutMs, String* errCode, String* errMsg);
void endLearn();
bool tryBeginSend(String* errCode, String* errMsg);
void endSend();
uint32_t learnRemainingMs();
uint32_t learnTimeoutMs();

// ---------------------------------------------------------------------------
// Device database (persisted to /ir_db.json by storage.cpp)
// ---------------------------------------------------------------------------
size_t deviceCount();
std::vector<std::pair<String, DeviceSummary>> listDevices();
bool getDeviceSummary(const String& deviceId, DeviceSummary* out);
bool getAction(const String& deviceId, const String& action, IrAction* out);

// Creates the device when needed and creates/overwrites the action.
bool upsertAction(const String& deviceId, const String& label, const String& action,
                  const IrSignal& signal, bool* createdDevice, String* errMsg);
bool deleteDevice(const String& deviceId, String* errMsg);
bool deleteAction(const String& deviceId, const String& action, String* errMsg);

bool loadDatabase();
bool saveDatabase();

// ---------------------------------------------------------------------------
// Temporary capture buffer (RAM only, latest M5IR_CAPTURE_SLOTS signals)
// ---------------------------------------------------------------------------
// Returns the new capture id ("cap_xxxx") or an empty String on failure.
String storeCapture(const IrSignal& signal);
bool getCapture(const String& captureId, IrSignal* out);

// Drops expired capture entries (called by tick()).
void cleanupCaptures();

// ---------------------------------------------------------------------------
// Relay status cache (updated by relay_ws.cpp, read by /api/health)
// ---------------------------------------------------------------------------
void setRelayStatus(bool enabled, bool connected);
bool relayEnabled();
bool relayConnected();

}  // namespace appstate
