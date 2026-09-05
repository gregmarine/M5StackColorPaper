/*
 * SPDX-License-Identifier: MIT
 */
#include "net/web_server.h"

#include <cstring>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

// dns_server.h uses esp_ip4_addr_t but the component keeps esp_netif private,
// so its header does not pull it in.
#include "esp_netif.h"

#include "dns_server.h"
#include "net/photo_api.h"
#include "net/wifi_ap.h"

// The web app, built by tools/build_webapp.py from photo_lab.html's #core and
// #shared blocks plus main/web/app.ui.html, and embedded gzipped. See
// main/CMakeLists.txt.
extern const uint8_t app_html_gz_start[] asm("_binary_app_html_gz_start");
extern const uint8_t app_html_gz_end[] asm("_binary_app_html_gz_end");

namespace net {
namespace {

constexpr const char* TAG = "web";

httpd_handle_t s_server       = nullptr;
dns_server_handle_t s_dns     = nullptr;
volatile int64_t s_last_request_us = 0;

void touch()
{
    s_last_request_us = esp_timer_get_time();
}

esp_err_t sendApp(httpd_req_t* req)
{
    touch();
    const size_t len = app_html_gz_end - app_html_gz_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // The page is rebuilt into the firmware on every flash and the frame has
    // no clock to validate against, so caching it would only ever serve a
    // stale app after an update.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(app_html_gz_start), len);
}

// Served to the captive-portal window instead of the real app.
//
// That window is not a browser in any useful sense: Android's is a WebView
// whose host app implements no file chooser, so the photo picker is simply
// dead, and iOS's suppresses dialogs. Rendering the manager there produces an
// app where half the buttons quietly do nothing. So the portal gets this
// instead -- a signpost, deliberately trivial enough to survive any WebView,
// with the address big enough to read and type if the button does not launch
// anything.
constexpr const char* PORTAL_LANDING_HTML = R"HTML(<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PaperColor</title>
<style>
 body{margin:0;background:#f3f2ee;color:#1d1d1b;
   font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif;
   display:flex;min-height:100vh;align-items:center;justify-content:center;padding:24px}
 .card{background:#fff;border:1px solid #d9d7d0;border-radius:14px;padding:28px 24px;max-width:420px;text-align:center}
 h1{font-size:20px;margin:0 0 6px}
 p{color:#6b6a66;margin:0 0 20px}
 a.go{display:block;background:#2f5fd6;color:#fff;text-decoration:none;font-weight:600;
   padding:16px;border-radius:10px;margin-bottom:20px}
 .addr{font-size:22px;font-weight:600;letter-spacing:.5px;
   background:#f3f2ee;border:1px solid #d9d7d0;border-radius:8px;padding:12px;word-break:break-all}
 .note{font-size:13px;color:#6b6a66;margin-top:16px}
</style>
<div class="card">
  <h1>PaperColor photo manager</h1>
  <p>You&rsquo;re connected. Open the manager in your normal browser.</p>
  <a class="go" href="http://192.168.4.1/" target="_blank" rel="noopener">Open the photo manager</a>
  <div class="addr">http://192.168.4.1</div>
  <p class="note">This Wi-Fi sign-in window can&rsquo;t open your photo library,
  so the manager runs in Chrome or Safari instead. If the button does nothing,
  type the address above.</p>
</div>
)HTML";

// The captive-portal window announces itself: iOS's browser carries
// CaptiveNetworkSupport, and Android's is a stock WebView, which is what the
// "; wv" marker means.
bool isRestrictedBrowser(httpd_req_t* req)
{
    const size_t len = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (len == 0 || len > 512) return false;
    char agent[513] = {};
    if (httpd_req_get_hdr_value_str(req, "User-Agent", agent, sizeof(agent)) != ESP_OK) {
        return false;
    }
    return std::strstr(agent, "CaptiveNetworkSupport") != nullptr ||
           std::strstr(agent, "; wv)") != nullptr ||
           std::strstr(agent, "; wv;") != nullptr;
}

esp_err_t sendPortalLanding(httpd_req_t* req)
{
    touch();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_LANDING_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t rootHandler(httpd_req_t* req)
{
    // The portal window follows its redirect to "/" too, so the check has to
    // be here and not only on the probe URLs.
    if (isRestrictedBrowser(req)) return sendPortalLanding(req);
    return sendApp(req);
}

// Phones decide they are behind a captive portal by fetching a known URL and
// checking they get exactly what they expected. Answering these with anything
// else is what makes the sign-in window open on its own -- which is worth
// keeping purely for discovery, since it is how you find the address without
// reading it off the panel. What it gets is the signpost, not the app.
esp_err_t portalRedirectHandler(httpd_req_t* req)
{
    return sendPortalLanding(req);
}

// Anything not otherwise routed. Unknown URLs are almost always a phone
// probing for a captive portal, so they get redirected to the app.
//
// API paths are the exception and must not be: httpd_resp_send_err() routes
// through here, so an honest 404 from the photo API ("no such photo") would
// otherwise reach the web app as a 302 to the index page, and fetch() would
// report a baffling HTML-parsing failure instead of the real error.
esp_err_t notFoundHandler(httpd_req_t* req, httpd_err_code_t /*err*/)
{
    if (std::strncmp(req->uri, "/api/", 5) == 0 || std::strncmp(req->uri, "/photo/", 7) == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
    }
    return portalRedirectHandler(req);
}

const httpd_uri_t URIS[] = {
    {.uri = "/", .method = HTTP_GET, .handler = rootHandler, .user_ctx = nullptr},
    // Android
    {.uri = "/generate_204", .method = HTTP_GET, .handler = portalRedirectHandler, .user_ctx = nullptr},
    {.uri = "/gen_204", .method = HTTP_GET, .handler = portalRedirectHandler, .user_ctx = nullptr},
    // iOS / macOS
    {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = portalRedirectHandler, .user_ctx = nullptr},
    {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = portalRedirectHandler, .user_ctx = nullptr},
    // Windows
    {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = portalRedirectHandler, .user_ctx = nullptr},
    {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = portalRedirectHandler, .user_ctx = nullptr},
};

}  // namespace

esp_err_t webStart()
{
    if (s_server != nullptr) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(URIS) / sizeof(URIS[0]) + 12;  // + the photo API
    config.lru_purge_enable = true;
    config.stack_size       = 8192;   // BMP uploads run on this task
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        s_server = nullptr;
        return err;
    }

    for (const auto& uri : URIS) {
        httpd_register_uri_handler(s_server, &uri);
    }
    if (photoApiRegister(s_server) != ESP_OK) {
        ESP_LOGE(TAG, "could not register the photo API");
        httpd_stop(s_server);
        s_server = nullptr;
        return ESP_FAIL;
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, notFoundHandler);

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns                       = start_dns_server(&dns_cfg);
    if (s_dns == nullptr) {
        // The app is still reachable by typing the address the panel shows, so
        // this is a degraded portal, not a failed start.
        ESP_LOGW(TAG, "DNS responder failed to start; captive portal will not pop up");
    }

    touch();
    ESP_LOGI(TAG, "HTTP server up on port %d", config.server_port);
    return ESP_OK;
}

void webStop()
{
    if (s_dns != nullptr) {
        stop_dns_server(s_dns);
        s_dns = nullptr;
    }
    if (s_server != nullptr) {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    ESP_LOGI(TAG, "HTTP server down");
}

void webTouch()
{
    touch();
}

uint32_t webMsSinceLastRequest()
{
    return static_cast<uint32_t>((esp_timer_get_time() - s_last_request_us) / 1000);
}

}  // namespace net
