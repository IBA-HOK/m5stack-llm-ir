// M5Stack LLM smart IR remote (MVP) - firmware entry point.
//
// Responsibilities (spec section 2, "hardware control layer"):
//   * Wi-Fi + mDNS bring-up
//   * local HTTP API (http_server.cpp)
//   * optional Cloudflare relay WebSocket (relay_ws.cpp)
//   * LittleFS persistence (storage.cpp), IR engine (ir_engine.cpp)
//   * LCD feedback and the global state machine (lcd_ui.cpp / state.cpp)
#include <Arduino.h>
#include <ESPmDNS.h>
#include <M5Unified.h>
#include <WiFi.h>

#include "config.h"
#include "http_server.h"
#include "ir_engine.h"
#include "lcd_ui.h"
#include "relay_ws.h"
#include "state.h"
#include "storage.h"

namespace {

bool s_mdnsStarted = false;
bool s_httpStarted = false;
uint32_t s_lastWifiCheckMs = 0;
uint32_t s_nextWifiReconnectMs = 0;

void startMdns() {
  if (s_mdnsStarted) return;
  if (MDNS.begin(M5IR_HOSTNAME)) {
    MDNS.addService("http", "tcp", M5IR_HTTP_PORT);
    MDNS.addServiceTxt("http", "tcp", "version", M5IR_VERSION);
    s_mdnsStarted = true;
    Serial.printf("[net] mDNS: http://%s.local/\n", M5IR_HOSTNAME);
  } else {
    Serial.println("[net] mDNS start failed");
  }
}

void initWifiStack() {
  // The lwIP/TCP-IP stack (needed by the HTTP server) is brought up by
  // WiFi.mode(), even when no credentials are configured yet.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(M5IR_HOSTNAME);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
}

bool connectWifi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("[net] WIFI_SSID is empty; copy .env.example to .env and fill it in");
    return false;
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (millis() - startedAt < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

void logNetworkState() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[net] Wi-Fi connected: ssid=%s ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[net] Wi-Fi not connected");
  }
}

void maintainWifi(uint32_t now) {
  if (now - s_lastWifiCheckMs < 5000) return;
  s_lastWifiCheckMs = now;

  if (WiFi.status() == WL_CONNECTED) {
    startMdns();
    if (appstate::state() == RunState::Error) {
      appstate::setState(RunState::Idle);
      appstate::setStatusMessage("Wi-Fi restored");
      logNetworkState();
    }
    return;
  }

  s_mdnsStarted = false;
  if (appstate::state() != RunState::Error) {
    appstate::setState(RunState::Error);
  }
  appstate::setStatusMessage("Wi-Fi disconnected; retrying");
  if (now >= s_nextWifiReconnectMs) {
    Serial.println("[net] Wi-Fi reconnect attempt");
    if (strlen(WIFI_SSID) == 0) {
      initWifiStack();
    } else {
      WiFi.reconnect();
    }
    s_nextWifiReconnectMs = now + 10000;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.printf("\n[boot] M5Stack LLM IR remote v%s\n", M5IR_VERSION);
  Serial.printf("[boot] hostname=%s.local ir_hw=%d tx=%d rx=%d\n", M5IR_HOSTNAME, M5IR_ENABLE_IR_HW,
                M5IR_IR_TX_PIN, M5IR_IR_RX_PIN);
  if (strlen(M5IR_API_KEY) == 0) {
    Serial.println("[boot] WARNING: M5IR_API_KEY is empty; authenticated endpoints return 503");
  }

  auto m5Config = M5.config();
  M5.begin(m5Config);
  Serial.printf("[boot] display=%dx%d board=%d\n", static_cast<int>(M5.Display.width()),
                static_cast<int>(M5.Display.height()), static_cast<int>(M5.getBoard()));

  appstate::begin();
  lcdui::begin();
  appstate::setState(RunState::Connecting);
  appstate::setStatusMessage("starting storage");

  if (!storage::begin()) {
    appstate::setStatusMessage("LittleFS mount failed");
  }
  appstate::loadDatabase();
  irengine::begin();

  appstate::setStatusMessage("connecting to Wi-Fi");
  initWifiStack();
  const bool wifiOk = connectWifi(20000);
  logNetworkState();
  if (wifiOk) {
    startMdns();
    appstate::setState(RunState::Idle);
    appstate::setStatusMessage("ready");
  } else {
    appstate::setState(RunState::Error);
    appstate::setStatusMessage("Wi-Fi unavailable; will retry");
  }

  s_httpStarted = httpserver::begin();
  if (!s_httpStarted) {
    Serial.println("[boot] HTTP server failed to start");
    appstate::setStatusMessage("HTTP server failed to start");
  }

  relayws::begin();

  Serial.printf("[boot] devices=%u ir_ready=%d http=%d relay=%d\n",
                static_cast<unsigned>(appstate::deviceCount()), irengine::hardwareReady(),
                s_httpStarted, relayws::enabled());
}

void loop() {
  M5.update();
  lcdui::loop();
  relayws::loop();
  appstate::tick();
  maintainWifi(millis());
  delay(10);
}
