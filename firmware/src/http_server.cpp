#include "http_server.h"

#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "api_handler.h"
#include "config.h"

namespace httpserver {
namespace {

httpd_handle_t s_server = nullptr;

const char* kStatusPage = R"rawhtml(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5 IR Bridge</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:24px;background:#10141c;color:#dfe7f2}
h1{font-size:18px;margin:0 0 12px;color:#7fd3ff}
pre{background:#181f2b;border-radius:8px;padding:12px;overflow:auto;font-size:12px;line-height:1.5}
.ok{color:#63e6a5}.bad{color:#ff7d7d}
</style></head><body>
<h1>M5Stack LLM IR Bridge</h1>
<div id="status" class="ok">connecting...</div>
<pre id="health"></pre>
<script>
async function poll(){
  const el=document.getElementById('health');
  try{
    const r=await fetch('/api/health',{cache:'no-store'});
    const j=await r.json();
    document.getElementById('status').textContent='online - state: '+j.state;
    el.textContent=JSON.stringify(j,null,2);
  }catch(e){
    document.getElementById('status').textContent='offline';
    document.getElementById('status').className='bad';
  }
}
poll(); setInterval(poll,2000);
</script>
</body></html>)rawhtml";

void setCommonHeaders(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

void sendJson(httpd_req_t* req, int status, const String& body) {
  char statusLine[48];
  snprintf(statusLine, sizeof(statusLine), "%d %s", status, api::statusText(status));
  httpd_resp_set_status(req, statusLine);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  setCommonHeaders(req);
  httpd_resp_send(req, body.c_str(), body.length());
}

void sendError(httpd_req_t* req, int status, const char* code, const String& message) {
  JsonDocument doc;
  doc["ok"] = false;
  JsonObject error = doc["error"].to<JsonObject>();
  error["code"] = code;
  error["message"] = message;
  String body;
  serializeJson(doc, body);
  sendJson(req, status, body);
}

const char* methodName(httpd_method_t method) {
  switch (method) {
    case HTTP_GET:
      return "GET";
    case HTTP_POST:
      return "POST";
    case HTTP_PUT:
      return "PUT";
    case HTTP_PATCH:
      return "PATCH";
    case HTTP_DELETE:
      return "DELETE";
    case HTTP_OPTIONS:
      return "OPTIONS";
    default:
      return "GET";
  }
}

bool readRequestBody(httpd_req_t* req, String* out, int* errorStatus) {
  const int contentLength = req->content_len;
  if (contentLength > M5IR_MAX_HTTP_BODY) {
    *errorStatus = 413;
    return false;
  }
  out->reserve(static_cast<unsigned>(contentLength) + 1);
  int remaining = contentLength;
  char buffer[256];
  while (remaining > 0) {
    const int chunk = (remaining < static_cast<int>(sizeof(buffer))) ? remaining : static_cast<int>(sizeof(buffer));
    const int received = httpd_req_recv(req, buffer, chunk);
    if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (received <= 0) {
      *errorStatus = 400;
      return false;
    }
    out->concat(buffer, received);
    remaining -= received;
  }
  return true;
}

bool isAuthorized(httpd_req_t* req) {
  char header[192];
  if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) {
    return false;
  }
  const String expected = String("Bearer ") + M5IR_API_KEY;
  return expected == header;
}

struct HttpJob {
  httpd_req_t* asyncReq = nullptr;
  ApiRequest request;
};

void apiTask(void* arg) {
  HttpJob* job = static_cast<HttpJob*>(arg);
  const ApiResponse response = api::handle(job->request);
  sendJson(job->asyncReq, response.status, response.body);
  httpd_req_async_handler_complete(job->asyncReq);
  delete job;
  vTaskDelete(nullptr);
}

esp_err_t apiHandler(httpd_req_t* req) {
  const String path = req->uri;

  if (req->method == HTTP_OPTIONS) {
    httpd_resp_set_status(req, "204 No Content");
    setCommonHeaders(req);
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }

  // GET /api/health is the only endpoint where auth is optional (spec 5.2).
  const bool isHealth = (path == "/api/health");
  if (!isHealth) {
    if (strlen(M5IR_API_KEY) == 0) {
      sendError(req, 503, "CONFIG_MISSING", "M5IR_API_KEY is not configured (fill in the root .env file)");
      return ESP_OK;
    }
    if (!isAuthorized(req)) {
      sendError(req, 401, "UNAUTHORIZED", "missing or invalid Authorization: Bearer token");
      return ESP_OK;
    }
  }

  String body;
  int bodyError = 0;
  if (!readRequestBody(req, &body, &bodyError)) {
    if (bodyError == 413) {
      sendError(req, 413, "PAYLOAD_TOO_LARGE",
                String("request body exceeds ") + M5IR_MAX_HTTP_BODY + " bytes");
    } else {
      sendError(req, 400, "INVALID_REQUEST", "failed to read the request body");
    }
    return ESP_OK;
  }

  HttpJob* job = new HttpJob();
  if (job == nullptr) {
    sendError(req, 500, "INTERNAL_ERROR", "out of memory");
    return ESP_OK;
  }
  job->request.method = methodName(static_cast<httpd_method_t>(req->method));
  job->request.path = path;
  job->request.body = body;

  httpd_req_t* asyncReq = nullptr;
  const esp_err_t asyncResult = httpd_req_async_handler_begin(req, &asyncReq);
  if (asyncResult != ESP_OK || asyncReq == nullptr) {
    // Fallback: handle synchronously if the async handler is unavailable.
    const ApiResponse response = api::handle(job->request);
    delete job;
    sendJson(req, response.status, response.body);
    return ESP_OK;
  }

  job->asyncReq = asyncReq;
  if (xTaskCreate(apiTask, "m5ir_api", M5IR_API_TASK_STACK, job, tskIDLE_PRIORITY + 2, nullptr) != pdPASS) {
    sendError(asyncReq, 500, "INTERNAL_ERROR", "could not spawn request handler");
    httpd_req_async_handler_complete(asyncReq);
    delete job;
  }
  return ESP_OK;
}

esp_err_t rootHandler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, kStatusPage, strlen(kStatusPage));
  return ESP_OK;
}

}  // namespace

bool begin() {
  if (s_server != nullptr) return true;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = M5IR_HTTP_PORT;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_open_sockets = 7;
  config.lru_purge_enable = true;
  config.stack_size = M5IR_HTTP_STACK_SIZE;

  if (httpd_start(&s_server, &config) != ESP_OK) {
    Serial.println("[http] failed to start server");
    s_server = nullptr;
    return false;
  }

  httpd_uri_t rootUri;
  memset(&rootUri, 0, sizeof(rootUri));
  rootUri.uri = "/";
  rootUri.method = HTTP_GET;
  rootUri.handler = rootHandler;
  rootUri.user_ctx = nullptr;
  httpd_register_uri_handler(s_server, &rootUri);

  httpd_uri_t apiUri;
  memset(&apiUri, 0, sizeof(apiUri));
  apiUri.uri = "/api/*";
  apiUri.method = static_cast<httpd_method_t>(HTTP_ANY);
  apiUri.handler = apiHandler;
  apiUri.user_ctx = nullptr;
  httpd_register_uri_handler(s_server, &apiUri);

  Serial.printf("[http] listening on port %d\n", M5IR_HTTP_PORT);
  return true;
}

void stop() {
  if (s_server == nullptr) return;
  httpd_stop(s_server);
  s_server = nullptr;
}

}  // namespace httpserver
