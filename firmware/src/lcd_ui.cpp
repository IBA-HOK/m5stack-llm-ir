#include "lcd_ui.h"

#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "ir_engine.h"
#include "state.h"

namespace lcdui {
namespace {

constexpr uint16_t kBackground = 0x0861;  // dark blue-ish
constexpr uint16_t kForeground = 0xFFFF;
constexpr uint16_t kMuted = 0x9CD3;
constexpr uint16_t kAccent = 0x2E7F;
constexpr uint16_t kOk = 0x2E66;
constexpr uint16_t kWarn = 0xFD20;
constexpr uint16_t kError = 0xF800;

bool s_available = false;
uint32_t s_lastUpdateMs = 0;
int s_scale = 1;
int s_lineHeight = 12;
String s_previousLines[8];

String fitText(const String& text, int maxChars) {
  if (maxChars <= 0) return String();
  if (static_cast<int>(text.length()) <= maxChars) return text;
  if (maxChars <= 1) return String();
  return text.substring(0, maxChars - 1) + "~";
}

void drawLine(int index, const String& text, uint16_t color, bool force = false) {
  if (index < 0 || index >= 8) return;
  if (!force && s_previousLines[index] == text) return;
  s_previousLines[index] = text;

  auto& display = M5.Display;
  const int width = display.width();
  const int y = 4 + index * s_lineHeight;
  const int maxChars = (width - 8) / (6 * s_scale);
  display.fillRect(0, y - 2, width, s_lineHeight, kBackground);
  display.setTextSize(s_scale);
  display.setTextColor(color, kBackground);
  display.setTextDatum(top_left);
  display.drawString(fitText(text, maxChars), 4, y);
}

uint16_t stateColor(RunState state) {
  switch (state) {
    case RunState::Idle:
      return kOk;
    case RunState::Learning:
    case RunState::Sending:
      return kWarn;
    case RunState::Error:
      return kError;
    default:
      return kMuted;
  }
}

// While LEARNING the screen shows *what* is being learned plus the remaining
// seconds, e.g. "LEARN: tv/ch1" / "Press remote! 10s".
void drawLearningStatus() {
  String target = appstate::learnTarget();
  if (target.isEmpty()) target = "(target not specified)";
  drawLine(6, "LEARN: " + target, kWarn, true);

  const uint32_t remainingMs = appstate::learnRemainingMs();
  const uint32_t seconds = (remainingMs + 999) / 1000;
  drawLine(7, "Press remote! " + String(seconds) + "s", kForeground, true);
}

}  // namespace

void begin() {
  auto& display = M5.Display;
  if (display.width() <= 0 || display.height() <= 0) {
    s_available = false;
    Serial.println("[ui] no display detected");
    return;
  }
  s_available = true;
  s_scale = (display.width() >= 260) ? 2 : 1;
  s_lineHeight = 8 * s_scale + 5;
  display.setBrightness(90);
  display.fillScreen(kBackground);
  display.setTextColor(kForeground, kBackground);
  for (String& line : s_previousLines) line = "";
}

void loop() {
  if (!s_available) return;
  const uint32_t now = millis();
  const RunState state = appstate::state();
  const bool learning = (state == RunState::Learning);
  const uint32_t interval = learning ? 200 : 500;
  if (now - s_lastUpdateMs < interval) return;
  s_lastUpdateMs = now;

  auto& display = M5.Display;
  display.startWrite();

  const bool wifiUp = (WiFi.status() == WL_CONNECTED);
  drawLine(0, String("M5 IR Bridge ") + M5IR_VERSION, kAccent, true);
  drawLine(1, String("STATE: ") + appstate::stateName(), stateColor(state), true);

  if (wifiUp) {
    drawLine(2, "WiFi " + WiFi.localIP().toString(), kForeground);
  } else {
    drawLine(2, "WiFi connecting...", kMuted);
  }

  String relayText;
  if (!appstate::relayEnabled()) {
    relayText = "Relay: OFF";
  } else if (appstate::relayConnected()) {
    relayText = "Relay: ON";
  } else {
    relayText = "Relay: RECONNECTING";
  }
  drawLine(3, relayText, appstate::relayConnected() ? kOk : kMuted);

  if (irengine::hardwareReady()) {
    drawLine(4, "IR unit: READY", kOk);
  } else {
    drawLine(4, "IR unit: NOT CONNECTED", kWarn);
  }

  drawLine(5, "Devices: " + String(static_cast<unsigned>(appstate::deviceCount())), kForeground);

  if (learning) {
    drawLearningStatus();
  } else {
    String message = appstate::statusMessage();
    if (message.isEmpty()) message = String(M5IR_HOSTNAME) + ".local";
    drawLine(6, message, kMuted);
    // Clear the countdown line when leaving LEARNING.
    drawLine(7, "", kMuted);
  }

  display.endWrite();
}

}  // namespace lcdui
