#pragma once

#include <esp_http_server.h>

#include <functional>
#include <string>

//==============================================================================
// HTTP SERVER SUPPORT
//==============================================================================
// esp_http_server takes plain C handlers with a void* context. These helpers add
// the small amount of glue the device page, JSON API and setup portal need:
// capturing lambdas as route handlers, plus request parsing and response
// shorthands.

namespace http {

/** A route handler. Returning anything but ESP_OK closes the connection. */
using Handler = std::function<esp_err_t(httpd_req_t*)>;

/**
 * Register a route.
 *
 * The handler is copied and kept alive for the lifetime of the process; routes
 * are registered once during start-up and never removed.
 */
bool route(httpd_handle_t server, const char* uri, httpd_method_t method, Handler handler);

/**
 * Register a WebSocket endpoint.
 *
 * The handler is called once with `req->method == HTTP_GET` after a successful
 * handshake, then once per received frame.
 */
bool websocket_route(httpd_handle_t server, const char* uri, Handler handler);

/** Register the handler invoked for any URI without a matching route. */
bool not_found(httpd_handle_t server, Handler handler);

//------------------------------------------------------------------------------
// Responses
//------------------------------------------------------------------------------

/** Send a complete response with an explicit status and content type. */
esp_err_t send(httpd_req_t* request, int status, const char* content_type,
               const std::string& body);

/** Send a JSON body. */
esp_err_t send_json(httpd_req_t* request, int status, const std::string& body);

/** Send a JSON `{"error": "..."}` body. */
esp_err_t send_error(httpd_req_t* request, int status, const char* message);

/** Send a 302 to `location`, used by the captive portal. */
esp_err_t send_redirect(httpd_req_t* request, const char* location);

/**
 * Stream a file from LittleFS.
 *
 * @param download When true, ask the browser to save rather than render it.
 */
esp_err_t send_file(httpd_req_t* request, const char* path, const char* content_type,
                    bool download);

//------------------------------------------------------------------------------
// Requests
//------------------------------------------------------------------------------

/** Read the whole request body, refusing anything larger than `max_bytes`. */
bool read_body(httpd_req_t* request, std::string& body, size_t max_bytes);

/** Look up a URL query parameter. */
bool query_param(httpd_req_t* request, const char* key, std::string& value);
bool has_query_param(httpd_req_t* request, const char* key);

/** Look up a field in an application/x-www-form-urlencoded body. */
bool form_field(const std::string& body, const char* key, std::string& value);
bool has_form_field(const std::string& body, const char* key);

/** Request header value, or an empty string when absent. */
std::string header(httpd_req_t* request, const char* name);

/**
 * Reject cross-origin writes.
 *
 * A request without an Origin header is allowed (curl, the Python tools); one
 * with an Origin must match the Host it was sent to.
 */
bool origin_allowed(httpd_req_t* request);

/** Decode application/x-www-form-urlencoded escaping, including '+' as space. */
std::string url_decode(const std::string& value);

/** Escape a value for embedding in a JSON string literal. */
std::string json_escape(const std::string& value);

}  // namespace http
