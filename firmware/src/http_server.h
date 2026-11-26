// Local HTTP server (spec section 4.1 / 5).
//
// Uses the ESP-IDF httpd server with asynchronous handlers so that a blocking
// POST /api/ir/learn never stalls unrelated requests. Concurrent learn/send
// calls are answered with 409 Conflict by the shared api_handler layer.
#pragma once

#include <Arduino.h>

namespace httpserver {

// Starts the server on M5IR_HTTP_PORT. Call once, after Wi-Fi is up.
bool begin();

// Stops the server (mainly useful for debugging / reconfiguration).
void stop();

}  // namespace httpserver
