#include "ir_engine.h"

#if M5IR_ENABLE_IR_HW

#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>

#include <cstring>

// =============================================================================
// Real Unit IR backend (M5Stack Unit IR U002, IRremoteESP8266 2.9.0).
//
// Design note
// -----------
// IRremoteESP8266 already contains decoders and transmitters for ~100 IR
// protocols (NEC/Sony/Samsung/..., and dozens of A/C protocols such as
// Mitsubishi, Daikin, Panasonic, Hitachi, Fujitsu, ...). We therefore do NOT
// implement per-protocol logic here. Instead:
//
//   * Receive: rely on IRrecv::decode(), which tries every compiled decoder.
//     `max_skip` lets the library skip leading noise entries (the red Grove
//     PORT.A picks up I2C noise), which used to make valid frames decode as
//     UNKNOWN.
//   * Transmit: dispatch through IRsend::send(decode_type_t, ...), the
//     library's own protocol dispatcher, so every decodable protocol is also
//     re-transmittable (A/C protocols use their 14-53 byte state arrays).
//   * Only genuinely unknown protocols fall back to the raw microsecond array,
//     converted exactly the same way as IRutils::resultToRawArray().
// =============================================================================

namespace {

IRsend s_sender(M5IR_IR_TX_PIN);

// IRrecvDumpV2 reference settings:
//  * 50ms timeout so multi-frame A/C packets are not cut apart.
//  * save_buffer=true so decoding works on a snapshot while reception continues.
constexpr uint8_t kCaptureTimeoutMs = 50;
constexpr bool kUseSaveBuffer = true;

// The original Core's red Grove (PORT.A) shares the internal I2C bus, so the
// first few captured entries are often noise/garbage. The library can search
// further into the buffer: offsets 1, 3, 5, ..., (2*10 + 1).
constexpr uint8_t kMaxNoiseSkip = 10;

IRrecv s_receiver(M5IR_IR_RX_PIN, M5IR_IR_RECV_BUFFER, kCaptureTimeoutMs,
                  kUseSaveBuffer);

bool s_ready = false;
const char* s_reason = "IR hardware is not initialised";

bool copyRawTimings(const decode_results& results, IrSignal* out,
                    String* errorCode);

// Convert a decoded (or undecoded) IRrecv result into our transport-neutral
// IrSignal. Multi-byte A/C protocols keep their full state[] so that the
// library's native transmitter can regenerate exact timings and checksums.
// A real decode must roughly match the capture length. Without this check a
// long A/C frame can be mis-matched as an 8-bit protocol at a shifted offset
// (e.g. MULTIBRACKETS with bits=8 but rawlen=691).
bool decodeLengthPlausible(const decode_results& results) {
  if (results.bits == 0) return false;
  const uint32_t expected = 2UL * results.bits;
  if (hasACState(results.decode_type)) {
    return results.rawlen >= expected;  // at least one full state frame
  }
  return results.rawlen + 8 >= expected && results.rawlen <= expected + 24;
}

bool decodeToSignal(const decode_results& results, IrSignal* out,
                    String* errorCode) {
  const decode_type_t type = results.decode_type;
  if (type != decode_type_t::UNKNOWN && decodeLengthPlausible(results)) {
    const String name = typeToString(type);
    out->freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
    out->raw.clear();
    out->state.clear();

    if (hasACState(type)) {
      const uint16_t nbytes = results.bits / 8;
      if (nbytes == 0 || nbytes > kStateSizeMax) return false;
      out->format = IrSignal::Format::Protocol;
      out->protocol = name;
      out->code = 0;
      out->bits = results.bits;
      out->state.assign(results.state, results.state + nbytes);
      return true;
    }
    if (results.bits <= 64) {
      out->format = IrSignal::Format::Protocol;
      out->protocol = name;
      out->code = results.value;
      out->bits = results.bits;
      return true;
    }
    return false;
  }

  // Unknown protocol -> keep the raw timings so it can still be replayed.
  return copyRawTimings(results, out, errorCode);
}

// Convert IRrecv's 2us ticks into the uint16_t microsecond array required by
// IRsend::sendRaw(). Mirrors IRutils::resultToRawArray(), including splitting
// durations longer than UINT16_MAX into mark/space pairs. Index 0 is the
// pre-message gap and is never transmitted.
bool copyRawTimings(const decode_results& results, IrSignal* out,
                    String* errorCode) {
  if (results.overflow || results.rawlen < 4) {
    if (errorCode != nullptr) *errorCode = "IR_UNSUPPORTED_SIGNAL";
    return false;
  }

  const uint16_t correctedLength = getCorrectedRawLength(&results);
  if (correctedLength > M5IR_MAX_RAW_LEN) {
    if (errorCode != nullptr) *errorCode = "IR_RAW_TOO_LONG";
    return false;
  }

  std::vector<uint16_t> raw;
  raw.reserve(correctedLength);
  for (uint16_t i = 1; i < results.rawlen; ++i) {
    uint32_t usecs = static_cast<uint32_t>(results.rawbuf[i]) * kRawTick;
    while (usecs > UINT16_MAX) {
      raw.push_back(UINT16_MAX);
      raw.push_back(0);  // 0 = "skip" marker in IRsend::sendRaw() arrays.
      usecs -= UINT16_MAX;
    }
    raw.push_back(static_cast<uint16_t>(usecs));
  }

  // Drop leading noise pairs (mark+space) that can precede the real frame on
  // this board. Only the prefix is touched; the frame body is never altered.
  size_t firstMark = 0;
  while (firstMark + 4 < raw.size() &&
         (raw[firstMark] > 20000 || raw[firstMark + 1] > 20000) &&
         raw[firstMark + 2] >= 300 && raw[firstMark + 2] <= 9000) {
    firstMark += 2;
  }
  if (firstMark > 0) raw.erase(raw.begin(), raw.begin() + firstMark);

  // Long captures are multi-frame A/C packets. Keep one complete frame
  // (split on >8ms inter-frame gaps) instead of replaying a partial/merged
  // stream, which appliances reject.
  if (raw.size() > 200) {
    std::vector<uint16_t> best;
    size_t start = 0;
    for (size_t i = 1; i < raw.size(); i += 2) {  // spaces sit at odd indexes
      if (raw[i] > 8000) {
        if (i - start > best.size()) {
          best.assign(raw.begin() + start, raw.begin() + i);
        }
        start = i + 1;
      }
    }
    if (raw.size() - start > best.size()) {
      best.assign(raw.begin() + start, raw.end());
    }
    if (best.size() >= 24) raw.swap(best);
  }

  // Very short UNKNOWN bursts on PORT.A are background noise, not key presses.
  if (raw.size() < 24 || raw[0] < 100 || raw[0] > 9000) {
    if (errorCode != nullptr) *errorCode = "IR_UNSUPPORTED_SIGNAL";
    return false;
  }

  out->format = IrSignal::Format::Raw;
  out->protocol = "";
  out->code = 0;
  out->bits = 0;
  out->freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
  out->state.clear();
  out->raw.swap(raw);
  return true;
}

}  // namespace

namespace irengine {

bool begin() {
  s_ready = false;
  if (M5IR_IR_TX_PIN < 0 || M5IR_IR_RX_PIN < 0) {
    s_reason = "IR GPIO pins are not configured for this board";
    Serial.printf("[ir] %s\n", s_reason);
    return false;
  }
  s_sender.begin();
  s_receiver.setUnknownThreshold(12);  // IRrecvDumpV2 default
  s_receiver.setTolerance(25);
  s_receiver.enableIRIn();
  s_ready = true;
  s_reason = "";
  Serial.printf(
      "[ir] Unit IR ready (TX=GPIO%d RX=GPIO%d buffer=%u timeout=%ums "
      "save_buffer=1 max_skip=%u)\n",
      M5IR_IR_TX_PIN, M5IR_IR_RX_PIN, M5IR_IR_RECV_BUFFER, kCaptureTimeoutMs,
      kMaxNoiseSkip);
  return true;
}

bool hardwareReady() { return s_ready; }

const char* unavailableReason() { return s_reason; }

const char* backendName() { return "irremote"; }

bool capture(uint32_t timeoutMs, IrSignal* out, String* errorCode,
             String* errorMessage) {
  if (!s_ready) {
    if (errorCode != nullptr) *errorCode = "IR_NOT_AVAILABLE";
    if (errorMessage != nullptr) *errorMessage = s_reason;
    return false;
  }
  Serial.printf("[ir] learning for %u ms\n", static_cast<unsigned>(timeoutMs));
  s_receiver.enableIRIn();
  decode_results results;
  memset(&results, 0, sizeof(results));

  const uint32_t startedAt = millis();
  while ((millis() - startedAt) < timeoutMs) {
    // max_skip=10 lets the library skip leading noise entries before it tries
    // its protocol decoders (offsets 1..21 in 2-entry steps).
    if (s_receiver.decode(&results, nullptr, kMaxNoiseSkip, 0)) {
      if (results.repeat) {
        s_receiver.resume();
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      const String name = typeToString(results.decode_type);
      String decodeError;
      const bool decoded = decodeToSignal(results, out, &decodeError);
      s_receiver.resume();
      if (!decoded) {
        if (errorCode != nullptr) *errorCode = decodeError;
        if (errorMessage != nullptr)
          *errorMessage = "captured signal was rejected, waiting for a cleaner frame";
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      out->rawEntries =
          (results.rawlen > 1) ? static_cast<uint16_t>(results.rawlen - 1) : 0;
      Serial.printf("[ir] decode=%s bits=%u rawlen=%u\n", name.c_str(),
                    static_cast<unsigned>(results.bits),
                    static_cast<unsigned>(results.rawlen));
      Serial.printf("[ir] captured %s (%u raw entries)\n",
                    out->format == IrSignal::Format::Protocol
                        ? out->protocol.c_str()
                        : "RAW",
                    static_cast<unsigned>(out->rawLength()));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  s_receiver.resume();
  if (errorCode != nullptr) *errorCode = "LEARN_TIMEOUT";
  if (errorMessage != nullptr) *errorMessage =
      "no infrared signal was received before the timeout";
  return false;
}

bool send(const IrSignal& signal, String* errorCode, String* errorMessage) {
  if (!s_ready) {
    if (errorCode != nullptr) *errorCode = "IR_NOT_AVAILABLE";
    if (errorMessage != nullptr) *errorMessage = s_reason;
    return false;
  }
  if (signal.empty()) {
    if (errorCode != nullptr) *errorCode = "IR_INVALID_SIGNAL";
    if (errorMessage != nullptr) *errorMessage = "signal is empty";
    return false;
  }

  s_receiver.disableIRIn();
  bool ok = false;

  if (signal.format == IrSignal::Format::Protocol) {
    const decode_type_t type = strToDecodeType(signal.protocol.c_str());
    if (type == decode_type_t::UNKNOWN) {
      if (errorCode != nullptr) *errorCode = "IR_UNSUPPORTED_PROTOCOL";
      if (errorMessage != nullptr)
        *errorMessage = "unknown protocol name: " + signal.protocol;
    } else if (!signal.state.empty()) {
      // Multi-byte A/C protocol: use the library's own transmitter, which
      // regenerates the exact header/bit timings and protocol checksum.
      Serial.printf("[ir] send state protocol=%s bytes=%u\n",
                    signal.protocol.c_str(),
                    static_cast<unsigned>(signal.state.size()));
      ok = s_sender.send(type, signal.state.data(),
                         static_cast<uint16_t>(signal.state.size()));
      if (ok) {
        // A/C remotes normally repeat the identical state frame 2-3 times.
        delay(40);
        s_sender.send(type, signal.state.data(),
                      static_cast<uint16_t>(signal.state.size()));
      }
    } else {
      Serial.printf("[ir] send protocol=%s code=0x%llX bits=%u\n",
                    signal.protocol.c_str(),
                    static_cast<unsigned long long>(signal.code),
                    static_cast<unsigned>(signal.bits));
      ok = s_sender.send(type, signal.code, signal.bits, 0);
    }
    if (!ok) {
      if (errorCode != nullptr) *errorCode = "IR_UNSUPPORTED_PROTOCOL";
      if (errorMessage != nullptr)
        *errorMessage = "protocol is not sendable: " + signal.protocol;
    }
  } else {
    if (signal.raw.size() > M5IR_MAX_RAW_LEN) {
      if (errorCode != nullptr) *errorCode = "IR_INVALID_SIGNAL";
      if (errorMessage != nullptr) *errorMessage = "raw signal is too long";
    } else {
      const uint16_t freq = (signal.freqKhz >= 20 && signal.freqKhz <= 60)
                                ? signal.freqKhz
                                : M5IR_IR_DEFAULT_FREQ_KHZ;
      Serial.printf("[ir] sendRaw len=%u freq=%uHz head=%u/%u/%u/%u\n",
                    static_cast<unsigned>(signal.raw.size()), freq * 1000U,
                    signal.raw.size() > 0 ? signal.raw[0] : 0,
                    signal.raw.size() > 1 ? signal.raw[1] : 0,
                    signal.raw.size() > 2 ? signal.raw[2] : 0,
                    signal.raw.size() > 3 ? signal.raw[3] : 0);
      s_sender.sendRaw(signal.raw.data(),
                       static_cast<uint16_t>(signal.raw.size()),
                       static_cast<uint16_t>(freq * 1000U));
      if (signal.raw.size() > 200) {
        for (int repeat = 0; repeat < 2; ++repeat) {
          delay(20);
          s_sender.sendRaw(signal.raw.data(),
                           static_cast<uint16_t>(signal.raw.size()),
                           static_cast<uint16_t>(freq * 1000U));
        }
      }
      ok = true;
    }
  }

  delay(50);
  s_receiver.enableIRIn();
  return ok;
}

}  // namespace irengine

#elif M5IR_FAKE_IR_BACKEND

// Synthetic backend for testing the API lifecycle before real IR hardware.
namespace {
const char* const kFakeReason = "";
}

namespace irengine {

bool begin() {
  Serial.println("[ir] FAKE backend enabled (test build, no real IR hardware)");
  return true;
}

bool hardwareReady() { return true; }

const char* unavailableReason() { return kFakeReason; }

const char* backendName() { return "fake"; }

bool capture(uint32_t timeoutMs, IrSignal* out, String* errorCode,
             String* errorMessage) {
  Serial.printf("[ir] fake capture for %u ms\n", static_cast<unsigned>(timeoutMs));
  vTaskDelay(pdMS_TO_TICKS(300));
  out->format = IrSignal::Format::Protocol;
  out->protocol = "NEC";
  out->code = 0x20DF10EFULL;
  out->bits = 32;
  out->freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
  out->raw.clear();
  out->state.clear();
  out->rawEntries = 67;
  if (errorCode != nullptr) errorCode->clear();
  if (errorMessage != nullptr) errorMessage->clear();
  return true;
}

bool send(const IrSignal& signal, String* errorCode, String* errorMessage) {
  (void)signal;
  if (errorCode != nullptr) errorCode->clear();
  if (errorMessage != nullptr) errorMessage->clear();
  return true;
}

}  // namespace irengine

#else  // M5IR_ENABLE_IR_HW

namespace {
const char* const kStubReason =
    "IR unit is not connected (firmware built without M5IR_ENABLE_IR_HW=1)";
}

namespace irengine {

bool begin() {
  Serial.printf("[ir] stub backend: %s\n", kStubReason);
  return false;
}

bool hardwareReady() { return false; }

const char* unavailableReason() { return kStubReason; }

const char* backendName() { return "stub"; }

bool capture(uint32_t, IrSignal*, String* errorCode, String* errorMessage) {
  if (errorCode != nullptr) *errorCode = "IR_NOT_AVAILABLE";
  if (errorMessage != nullptr) *errorMessage = kStubReason;
  return false;
}

bool send(const IrSignal&, String* errorCode, String* errorMessage) {
  if (errorCode != nullptr) *errorCode = "IR_NOT_AVAILABLE";
  if (errorMessage != nullptr) *errorMessage = kStubReason;
  return false;
}

}  // namespace irengine

#endif  // M5IR_ENABLE_IR_HW
