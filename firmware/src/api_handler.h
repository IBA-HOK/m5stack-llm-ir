// Transport-independent command layer.
//
// The local HTTP server and the Cloudflare relay WebSocket client both call
// api::handle() so that both transports share exactly the same routing,
// validation, state machine and JSON shapes (spec section 2).
#pragma once

#include <Arduino.h>

struct ApiRequest {
  String method;  // "GET", "POST", "DELETE", ...
  String path;    // "/api/..." (no query string)
  String body;    // raw request body (may be empty)
};

struct ApiResponse {
  int status = 200;
  String body;  // serialized JSON
};

namespace api {

// Human readable reason phrase for an HTTP status code ("OK", "Not Found", ...).
const char* statusText(int status);

// Handles one request. Authentication is the transport's responsibility.
ApiResponse handle(const ApiRequest& request);

}  // namespace api
