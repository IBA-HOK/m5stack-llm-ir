// Compile-time configuration for the M5Stack LLM smart IR remote firmware.
//
// Secrets (Wi-Fi, API key, relay settings) live in the git-ignored `.env` file
// at the repository root. `firmware/extra_script.py` injects them as build
// macros (-DWIFI_SSID=... etc.) before compiling. See `.env.example`.
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Version / identity
// ---------------------------------------------------------------------------
#ifndef M5IR_VERSION
#define M5IR_VERSION "0.1.0"
#endif

// mDNS hostname: the device answers on http://<hostname>.local/
#ifndef M5IR_HOSTNAME
#define M5IR_HOSTNAME "m5ir"
#endif

// Local HTTP port (spec section 4.1: default 80).
#ifndef M5IR_HTTP_PORT
#define M5IR_HTTP_PORT 80
#endif

// ---------------------------------------------------------------------------
// IR hardware
//
// The M5Stack Unit IR (U002) is not connected yet, so the default build has
// M5IR_ENABLE_IR_HW == 0 and uses a stub IR engine. The HTTP API, relay
// tunnel, persistence and UI are fully functional; IR endpoints return
// 503 IR_NOT_AVAILABLE until an `-ir` PlatformIO environment is built.
//
// M5IR_FAKE_IR_BACKEND enables a synthetic IR backend (used by the `-test`
// environments) that lets the full API lifecycle (learn -> test -> save ->
// send -> delete) be exercised on real hardware before the Unit IR arrives.
// Never enable it in production builds.
// ---------------------------------------------------------------------------
#ifndef M5IR_ENABLE_IR_HW
#define M5IR_ENABLE_IR_HW 0
#endif
#ifndef M5IR_FAKE_IR_BACKEND
#define M5IR_FAKE_IR_BACKEND 0
#endif

// Grove pin numbers; overridden per board in platformio.ini.
#ifndef M5IR_IR_TX_PIN
#define M5IR_IR_TX_PIN -1
#endif
#ifndef M5IR_IR_RX_PIN
#define M5IR_IR_RX_PIN -1
#endif

// Capture buffer (in 2us ticks) handed to IRrecv. 1024 ticks is enough for
// long A/C frames and the raw fallback used for unknown protocols.
#ifndef M5IR_IR_RECV_BUFFER
#define M5IR_IR_RECV_BUFFER 1024
#endif

// Assumed carrier frequency for captured raw signals (IR capture does not
// report the carrier frequency).
#ifndef M5IR_IR_DEFAULT_FREQ_KHZ
#define M5IR_IR_DEFAULT_FREQ_KHZ 38
#endif

// ---------------------------------------------------------------------------
// API limits (spec sections 4.3 / 5)
// ---------------------------------------------------------------------------
#define M5IR_LEARN_TIMEOUT_DEFAULT_MS 10000UL
#define M5IR_LEARN_TIMEOUT_MAX_MS 30000UL
#define M5IR_LEARN_TIMEOUT_MIN_MS 500UL
#define M5IR_SEND_MAX_STEPS 20
#define M5IR_SEND_MAX_DELAY_MS 5000UL
#define M5IR_MAX_HTTP_BODY 8192
#define M5IR_HTTP_STACK_SIZE 10240
#define M5IR_API_TASK_STACK 12288

// Captured signals kept in RAM before they are persisted. The spec keeps the
// latest 1..3 captures for 10 minutes or until reboot.
#define M5IR_CAPTURE_SLOTS 3
#define M5IR_CAPTURE_TTL_MS (10UL * 60UL * 1000UL)

// Persistence
#define M5IR_DB_PATH "/ir_db.json"
#define M5IR_DB_TMP_PATH "/ir_db.tmp"
#define M5IR_DB_BAK_PATH "/ir_db.bak"
#define M5IR_DB_SCHEMA_VERSION 1
#define M5IR_DB_MAX_BYTES (256UL * 1024UL)
#define M5IR_MAX_DEVICES 64
#define M5IR_MAX_ACTIONS_PER_DEVICE 64
#define M5IR_MAX_RAW_LEN 1024
#define M5IR_MAX_ID_LEN 24
#define M5IR_MAX_ACTION_LEN 32
#define M5IR_MAX_LABEL_LEN 64
#define M5IR_MAX_PROTOCOL_LEN 16

// Relay (Cloudflare Workers reverse tunnel)
#define M5IR_RELAY_PING_INTERVAL_MS 30000UL
#define M5IR_RELAY_PONG_TIMEOUT_MS 45000UL
#define M5IR_RELAY_CONNECT_TIMEOUT_MS 15000UL
#define M5IR_RELAY_BACKOFF_MIN_MS 2000UL
#define M5IR_RELAY_BACKOFF_MAX_MS 30000UL

// ---------------------------------------------------------------------------
// Secrets. firmware/extra_script.py generates include/secrets_generated.h from
// the git-ignored root `.env` on every build. The defaults below keep the
// firmware building when that generated header is missing.
// ---------------------------------------------------------------------------
#if defined(__has_include)
#if __has_include("secrets_generated.h")
#include "secrets_generated.h"
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef M5IR_API_KEY
#define M5IR_API_KEY ""
#endif
#ifndef RELAY_WS_URL
#define RELAY_WS_URL ""
#endif
#ifndef DEVICE_TOKEN
#define DEVICE_TOKEN ""
#endif
