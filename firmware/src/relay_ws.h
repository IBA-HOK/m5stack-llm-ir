// Optional WSS reverse tunnel client for the Cloudflare Workers relay
// (spec section 6).
//
// The device keeps one outgoing WebSocket to the relay. The relay forwards
// HTTP requests as JSON frames; this client executes them through the exact
// same api::handle() used by the local HTTP server and returns the response.
#pragma once

#include <Arduino.h>

namespace relayws {

// Parses RELAY_WS_URL / DEVICE_TOKEN and starts the client (if configured).
// Returns false when the relay is disabled (empty URL or token).
bool begin();

bool enabled();
bool connected();

// Must be called from loop(): pumps the WebSocket, keep-alive and reconnects.
void loop();

}  // namespace relayws
