// Infrared engine: abstract interface over IRremoteESP8266.
//
// Two build modes:
//   * M5IR_ENABLE_IR_HW == 0 (default): stub backend. The Unit IR has not been
//     connected yet, so the HTTP/relay plumbing stays testable while IR
//     endpoints fail fast with 503 IR_NOT_AVAILABLE.
//   * M5IR_ENABLE_IR_HW == 1: real Unit IR (U002) backend using
//     IRremoteESP8266 (38 kHz LED + receiver).
#pragma once

#include <Arduino.h>

#include "config.h"
#include "state.h"

namespace irengine {

// Initialises the IR hardware. Returns false when IR is unavailable; the
// reason is available through unavailableReason().
bool begin();

// True only when the firmware was built with IR support AND begin() succeeded.
bool hardwareReady();

// Human readable reason why IR is unavailable (never nullptr).
const char* unavailableReason();

// Active backend: "irremote" (real Unit IR), "fake" (test build) or "stub".
const char* backendName();

// Blocking IR capture. Waits up to timeoutMs for a signal (including repeat
// frames that arrive during the window) and converts it to an IrSignal.
// On failure sets errorCode to "LEARN_TIMEOUT" or "IR_CAPTURE_FAILED".
bool capture(uint32_t timeoutMs, IrSignal* out, String* errorCode, String* errorMessage);

// Sends a stored signal (protocol or raw). Sets errorCode on failure.
bool send(const IrSignal& signal, String* errorCode, String* errorMessage);

}  // namespace irengine
