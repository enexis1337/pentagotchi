// Web UI server (SoftAP + esp_http_server). Compact single-page status UI
// inspired by the original pwnagotchi web UI, served with basic auth from
// ui.web.username/password. Toggled by ui.web.enabled in /config.json
// (default: off).

#include "pentagotchi_web.h"

#include "pentagotchi_app.h"
#include "pentagotchi_events.h"
#include "pentagotchi_grid.h"
#include "pentagotchi_internal.h"
#include "pwn_ui.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <string.h>

using namespace pentagotchi::detail;

namespace {

const char *kTag = "pentagotchi_web";

const pentagotchi_config_t *s_cfg = nullptr;
httpd_handle_t s_server = nullptr;

// ---------------------------------------------------------------------------
// basic auth
// ---------------------------------------------------------------------------

// Decode a base64 Basic-auth payload and compare against ui.web credentials.
// Returns true when the request is allowed.
bool authOk(httpd_req_t *req) {
    if (!s_cfg || !s_cfg->web.username[0]) {
        return true; // no credentials configured: open
    }

    const size_t hdrLen = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hdrLen == 0 || hdrLen > 256) {
        return false;
    }
    char hdr[260];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    const char *basic = "Basic ";
    if (strncmp(hdr, basic, strlen(basic)) != 0) {
        return false;
    }

    uint8_t decoded[64];
    size_t olen = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded), &olen,
                              (const unsigned char *)hdr + strlen(basic),
                              strlen(hdr) - strlen(basic)) != 0) {
        return false;
    }
    decoded[olen < sizeof(decoded) ? olen : sizeof(decoded) - 1] = '\0';

    char userpass[64];
    snprintf(userpass, sizeof(userpass), "%s:%s", s_cfg->web.username, s_cfg->web.password);
    return strcmp((const char *)decoded, userpass) == 0;
}

esp_err_t requireAuth(httpd_req_t *req) {
    if (authOk(req)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"pentagotchi\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// status JSON
// ---------------------------------------------------------------------------

static void statusJson(char *out, size_t cap) {
    PentagotchiApp *app = gInstance;
    const uint8_t ch = readWifiChannel();
    size_t currentChannelAps = 0;
    if (app) {
        portENTER_CRITICAL(&gRadioMux);
        for (const auto &entry : gRegisteredBeacons) {
            if (entry.channel == ch) {
                ++currentChannelAps;
            }
        }
        portEXIT_CRITICAL(&gRadioMux);
    }

    char uptime[16];
    snprintf(uptime, sizeof(uptime), "%u", app ? app->uptimeSec() : 0);

    String friendFace, friendName;
    uint32_t friendSession = 0, friendTotal = 0;
    int friendRssi = -1000;
    if (app) {
        pentagotchi_grid_closest_peer(friendFace, friendName, friendSession, friendTotal, friendRssi);
    }

    snprintf(out, cap,
             "{\"name\":\"%s\",\"mode\":\"AUTO\",\"channel\":%u,"
             "\"aps_current\":%zu,\"aps_total\":%lu,"
             "\"pwnd_session\":%d,\"pwnd_total\":%lu,"
             "\"uptime\":%s,"
             "\"face\":\"%s\",\"status\":\"%s\","
             "\"friend_face\":\"%s\",\"friend_name\":\"%s\",\"friend_rssi\":%d}",
             s_cfg ? s_cfg->name : "pentagotchi",
             ch,
             currentChannelAps,
             app ? (unsigned long)app->stats().total_aps : 0UL,
             gHandshakeCount,
             app ? (unsigned long)app->stats().total_pwnd : 0UL,
             uptime,
             pwn_ui_get_face(),
             pwn_ui_get_status(),
             friendFace.c_str(),
             friendName.c_str(),
             friendRssi);
}

// ---------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------

esp_err_t handleIndex(httpd_req_t *req) {
    if (requireAuth(req) != ESP_OK) {
        return ESP_OK;
    }
    static const char kPage[] =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>pentagotchi</title>"
        "<style>"
        "body{background:#111;color:#7fdb7f;font-family:monospace;padding:12px;max-width:480px;margin:0 auto;}"
        "h1{font-size:18px;margin:0 0 4px;}"
        ".face{font-size:30px;margin:8px 0;}"
        ".row{display:flex;justify-content:space-between;padding:3px 0;border-bottom:1px solid #222;}"
        ".row b{color:#fff;}"
        "#status{font-size:12px;color:#aaa;margin:8px 0 0;min-height:14px;}"
        "</style></head><body>"
        "<h1>pentagotchi <span id=\"mode\"></span></h1>"
        "<div class=\"face\" id=\"face\"></div>"
        "<div class=\"row\"><span>Channel</span><b id=\"channel\"></b></div>"
        "<div class=\"row\"><span>APs</span><b id=\"aps\"></b></div>"
        "<div class=\"row\"><span>PWND</span><b id=\"pwnd\"></b></div>"
        "<div class=\"row\"><span>Uptime</span><b id=\"uptime\"></b></div>"
        "<div class=\"row\"><span>Friend</span><b id=\"friend\"></b></div>"
        "<div id=\"status\"></div>"
        "<script>"
        "function refresh(){fetch('/api/status').then(r=>r.json()).then(d=>{"
        "document.title=d.name;"
        "document.querySelector('h1').firstChild.nodeValue=d.name+' ';"
        "document.getElementById('mode').textContent=d.mode;"
        "document.getElementById('face').textContent=d.face;"
        "document.getElementById('channel').textContent=d.channel;"
        "document.getElementById('aps').textContent=d.aps_current+' ('+d.aps_total+')';"
        "document.getElementById('pwnd').textContent=d.pwnd_session+' ('+d.pwnd_total+')';"
        "document.getElementById('uptime').textContent=d.uptime+'s';"
        "document.getElementById('friend').textContent=(d.friend_name?d.friend_name+' '+d.friend_rssi:'-');"
        "document.getElementById('status').textContent=d.status;"
        "}).catch(()=>{});}"
        "refresh();setInterval(refresh,2000);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleApiStatus(httpd_req_t *req) {
    if (requireAuth(req) != ESP_OK) {
        return ESP_OK;
    }
    char buf[512];
    statusJson(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleReboot(httpd_req_t *req) {
    if (requireAuth(req) != ESP_OK) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Rebooting...", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(kTag, "web reboot requested");
    esp_restart();
    return ESP_OK;
}

} // namespace

void pentagotchi_web_start(const pentagotchi_config_t *cfg) {
    if (s_server) {
        return;
    }
    s_cfg = cfg;

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port = 80;
    conf.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
        return;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handleIndex},
        {.uri = "/api/status", .method = HTTP_GET, .handler = handleApiStatus},
        {.uri = "/reboot", .method = HTTP_POST, .handler = handleReboot},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(kTag, "web UI: http://%s/ (auth %s/%s)", cfg->web.address,
             cfg->web.username[0] ? cfg->web.username : "(none)",
             cfg->web.username[0] ? "password" : "");
}

void pentagotchi_web_stop(void) {
    if (!s_server) {
        return;
    }
    httpd_stop(s_server);
    s_server = nullptr;
    s_cfg = nullptr;
}

bool pentagotchi_web_is_running(void) {
    return s_server != nullptr;
}
