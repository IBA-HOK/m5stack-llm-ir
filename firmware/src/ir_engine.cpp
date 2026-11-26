#include "ir_engine.h"

#if M5IR_ENABLE_IR_HW

#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>

namespace {

IRsend s_sender(M5IR_IR_TX_PIN);
IRrecv s_receiver(M5IR_IR_RX_PIN, M5IR_IR_RECV_BUFFER);

bool s_ready = false;
const char* s_reason = "IR hardware is not initialised";

// Protocols this firmware knows how to re-transmit from decoded values. Any
// other decoded protocol is deliberately captured as raw timings so that it
// can always be replayed later (spec 7.1 keeps known protocols compact).
bool isSendableProtocol(const String& protocol) {
  static const char* const kSupported[] = {
      "NEC",     "SONY",    "SAMSUNG", "RC5",    "RC5X",       "RC6",
      "PANASONIC", "JVC",   "LG",      "DISH",   "SHARP",      "MITSUBISHI",
      "MITSUBISHI2", "COOLIX", "WHYNTER", "AIWA_RC_T501", "MAGIQUEST",
      "RCMM",    "SANYO_LC7461",
  };
  for (const char* name : kSupported) {
    if (protocol.equalsIgnoreCase(name)) return true;
  }
  return false;
}

// Copies the captured raw timings (index 0 is the leading gap and is skipped,
// matching the library's own IRrecvDump examples) into microseconds.
bool copyRawTimings(const decode_results& results, IrSignal* out) {
  if (results.overflow) {
    return false;
  }
  const uint16_t rawlen = results.rawlen;
  if (rawlen < 4) return false;
  out->format = IrSignal::Format::Raw;
  out->protocol = "";
  out->code = 0;
  out->bits = 0;
  out->freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
  const uint16_t count = static_cast<uint16_t>(rawlen - 1);
  if (count > M5IR_MAX_RAW_LEN) return false;
  out->raw.clear();
  out->raw.reserve(count);
  for (uint16_t i = 1; i < rawlen; ++i) {
    const uint32_t usecs = static_cast<uint32_t>(results.rawbuf[i]) * kRawTick;
    if (usecs == 0 || usecs > 65535UL) return false;
    out->raw.push_back(static_cast<uint16_t>(usecs));
  }
  return !out->raw.empty();
}

bool decodeToSignal(const decode_results& results, IrSignal* out, String* errorCode) {
  if (results.decode_type != decode_type_t::UNKNOWN && results.bits > 0 &&
      results.bits <= 64) {
    const String name = typeToString(results.decode_type);
    if (isSendableProtocol(name)) {
      out->format = IrSignal::Format::Protocol;
      out->protocol = name;
      out->code = results.value;
      out->bits = results.bits;
      out->freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
      out->raw.clear();
      return true;
    }
  }

  if (!copyRawTimings(results, out)) {
    if (errorCode != nullptr) *errorCode = "IR_UNSUPPORTED_SIGNAL";
    return false;
  }
  return true;
}

bool dispatchProtocol(const IrSignal& signal) {
  const String& protocol = signal.protocol;
  const uint64_t code = signal.code;
  const uint16_t bits = signal.bits;
  const uint16_t repeat = 0;

  if (protocol.equalsIgnoreCase("NEC")) {
    s_sender.sendNEC(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("SONY")) {
    s_sender.sendSony(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("SAMSUNG")) {
    s_sender.sendSAMSUNG(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("RC5") || protocol.equalsIgnoreCase("RC5X")) {
    s_sender.sendRC5(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("RC6")) {
    s_sender.sendRC6(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("PANASONIC")) {
    s_sender.sendPanasonic64(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("JVC")) {
    s_sender.sendJVC(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("LG")) {
    s_sender.sendLG(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("DISH")) {
    s_sender.sendDISH(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("SHARP")) {
    s_sender.sendSharpRaw(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("MITSUBISHI")) {
    s_sender.sendMitsubishi(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("MITSUBISHI2")) {
    s_sender.sendMitsubishi2(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("COOLIX")) {
    s_sender.sendCOOLIX(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("WHYNTER")) {
    s_sender.sendWhynter(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("AIWA_RC_T501")) {
    s_sender.sendAiwaRCT501(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("MAGIQUEST")) {
    s_sender.sendMagiQuest(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("RCMM")) {
    s_sender.sendRCMM(code, bits, repeat);
  } else if (protocol.equalsIgnoreCase("SANYO_LC7461")) {
    s_sender.sendSanyoLC7461(code, bits, repeat);
  } else {
    return false;
  }
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
  s_receiver.setTolerance(25);
  s_receiver.enableIRIn();
  s_ready = true;
  s_reason = "";
  Serial.printf("[ir] Unit IR enabled (TX=GPIO%d, RX=GPIO%d)\n", M5IR_IR_TX_PIN, M5IR_IR_RX_PIN);
  return true;
}

bool hardwareReady() { return s_ready; }

const char* unavailableReason() { return s_reason; }

const char* backendName() { return "irremote"; }

bool capture(uint32_t timeoutMs, IrSignal* out, String* errorCode, String* errorMessage) {
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
    if (s_receiver.decode(&results)) {
      if (results.repeat) {
        // A held-down button produces repeats; keep waiting for a real frame.
        s_receiver.resume();
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      String decodeError;
      const bool decoded = decodeToSignal(results, out, &decodeError);
      s_receiver.resume();
      if (!decoded) {
        if (errorCode != nullptr) *errorCode = decodeError;
        if (errorMessage != nullptr) *errorMessage = "captured signal could not be stored";
        return false;
      }
      out->rawEntries = (results.rawlen > 1) ? static_cast<uint16_t>(results.rawlen - 1) : 0;
      Serial.printf("[ir] captured %s (%u raw entries)\n",
                    out->format == IrSignal::Format::Protocol ? out->protocol.c_str() : "RAW",
                    static_cast<unsigned>(out->rawLength()));
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  s_receiver.resume();
  if (errorCode != nullptr) *errorCode = "LEARN_TIMEOUT";
  if (errorMessage != nullptr) *errorMessage = "no infrared signal was received before the timeout";
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
    ok = dispatchProtocol(signal);
    if (!ok) {
      if (errorCode != nullptr) *errorCode = "IR_UNSUPPORTED_PROTOCOL";
      if (errorMessage != nullptr) *errorMessage = "cannot re-transmit protocol " + signal.protocol;
    }
  } else {
    if (signal.raw.size() > M5IR_MAX_RAW_LEN) {
      if (errorCode != nullptr) *errorCode = "IR_INVALID_SIGNAL";
      if (errorMessage != nullptr) *errorMessage = "raw signal is too long";
    } else {
      const uint16_t freq = (signal.freqKhz >= 20 && signal.freqKhz <= 60)
                                ? signal.freqKhz
                                : M5IR_IR_DEFAULT_FREQ_KHZ;
      s_sender.sendRaw(signal.raw.data(), static_cast<uint16_t>(signal.raw.size()),
                       static_cast<uint16_t>(freq * 1000U));
      ok = true;
    }
  }
  delay(50);
  s_receiver.enableIRIn();
  return ok;
}

}  // namespace irengine

#elif M5IR_FAKE_IR_BACKEND

// Synthetic backend for testing the API lifecycle before the Unit IR arrives.
// It reports a fake NEC signal and pretends to transmit successfully.
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

bool capture(uint32_t timeoutMs, IrSignal* out, String* errorCode, String* errorMessage) {
  Serial.printf("[ir] fake capture for %u ms\n", static_cast<unsigned>(timeoutMs));
  vTaskDelay(pdMS_TO_TICKS(300));
  out->format = IrSignal::Format::Protocol;
  out->protocol = "NEC";
  out->code = 0x20DF10EFULL;
  out->bits = 32;
  out->freqKhz = M5IR_IR_DEFAULT_FREQ_KHZ;
  out->raw.clear();
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
