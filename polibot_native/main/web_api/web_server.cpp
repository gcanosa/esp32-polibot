// The web server itself: routes, the embedded pages and what every API handler shares
#include "web_server.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <iterator>

static const char* TAG = "WEB";

// Files compiled into the firmware (main/web), sent straight from flash
#define EMBEDDED(symbol)                                                    \
    extern const char symbol##_start[] asm("_binary_" #symbol "_start"); \
    extern const char symbol##_end[] asm("_binary_" #symbol "_end")
EMBEDDED(index_html);
EMBEDDED(home_html);
EMBEDDED(logo_webp);
EMBEDDED(board_webp);
EMBEDDED(theme_js);
EMBEDDED(ui_css);
EMBEDDED(panel_css);
EMBEDDED(panel_core_js);
EMBEDDED(panel_irc_js);
EMBEDDED(panel_rules_js);
EMBEDDED(panel_users_js);
EMBEDDED(panel_system_js);
EMBEDDED(panel_logs_js);
EMBEDDED(panel_wifi_js);
EMBEDDED(panel_access_js);

namespace {

struct Asset {
    const char* path;
    const char* type;
    const char* start;
    const char* end;
    bool text; // embedded text files end in a zero byte that is not part of them
};

const char* HTML = "text/html; charset=utf-8";
const char* JS = "text/javascript; charset=utf-8";

const Asset ASSETS[] = {
    {"/", HTML, home_html_start, home_html_end, true},
    {"/index.html", HTML, home_html_start, home_html_end, true},
    {"/panel", HTML, index_html_start, index_html_end, true},
    {"/ui.css", "text/css; charset=utf-8", ui_css_start, ui_css_end, true},
    {"/panel.css", "text/css; charset=utf-8", panel_css_start, panel_css_end, true},
    {"/panel_core.js", JS, panel_core_js_start, panel_core_js_end, true},
    {"/panel_irc.js", JS, panel_irc_js_start, panel_irc_js_end, true},
    {"/panel_rules.js", JS, panel_rules_js_start, panel_rules_js_end, true},
    {"/panel_users.js", JS, panel_users_js_start, panel_users_js_end, true},
    {"/panel_system.js", JS, panel_system_js_start, panel_system_js_end, true},
    {"/panel_logs.js", JS, panel_logs_js_start, panel_logs_js_end, true},
    {"/panel_wifi.js", JS, panel_wifi_js_start, panel_wifi_js_end, true},
    {"/panel_access.js", JS, panel_access_js_start, panel_access_js_end, true},
    {"/theme.js", JS, theme_js_start, theme_js_end, true},
    {"/logo.webp", "image/webp", logo_webp_start, logo_webp_end, false},
    {"/board.webp", "image/webp", board_webp_start, board_webp_end, false},
};

esp_err_t asset_handler(httpd_req_t* req) {
    const Asset& asset = *static_cast<const Asset*>(req->user_ctx);
    httpd_resp_set_type(req, asset.type);
    // pages and scripts must match the firmware that serves them; the logo hardly changes
    httpd_resp_set_hdr(req, "Cache-Control", asset.text ? "no-cache" : "public, max-age=86400");
    return httpd_resp_send(req, asset.start, asset.end - asset.start - (asset.text ? 1 : 0));
}

} // namespace

WebServer::WebServer(ConfigManager& config_mgr, SecurityManager& security_mgr, IRCManager& irc_mgr)
    : config_mgr_(config_mgr), security_mgr_(security_mgr), irc_mgr_(irc_mgr) {}

WebServer::~WebServer() {
    stop();
}

esp_err_t WebServer::send_json(httpd_req_t* req, cJSON* json, int status_code) {
    char* printed = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!printed) return ESP_FAIL;

    if (status_code == 401) httpd_resp_set_status(req, "401 Unauthorized");
    else if (status_code == 403) httpd_resp_set_status(req, "403 Forbidden");
    else if (status_code == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (status_code == 404) httpd_resp_set_status(req, "404 Not Found");
    else if (status_code == 409) httpd_resp_set_status(req, "409 Conflict");
    else if (status_code == 429) httpd_resp_set_status(req, "429 Too Many Requests");
    else if (status_code == 500) httpd_resp_set_status(req, "500 Internal Server Error");
    else httpd_resp_set_status(req, "200 OK");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, printed, strlen(printed));
    cJSON_free(printed);
    return res;
}

esp_err_t WebServer::send_message(httpd_req_t* req, int status_code, const std::string& message) {
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", status_code == 200 ? "ok" : "error");
    cJSON_AddStringToObject(res, "message", message.c_str());
    return send_json(req, res, status_code);
}

std::string WebServer::read_post_body(httpd_req_t* req, int limit) {
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > limit) return "";
    std::string body(total_len, '\0');
    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, &body[received], total_len - received);
        if (r <= 0) break;
        received += r;
    }
    return body;
}

std::string WebServer::get_token(httpd_req_t* req) {
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) == ESP_OK) {
        std::string h = auth_hdr;
        if (h.rfind("Bearer ", 0) == 0) return h.substr(7);
    }
    char token_hdr[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token_hdr, sizeof(token_hdr)) == ESP_OK) return token_hdr;
    return "";
}

bool WebServer::get_authenticated_user(httpd_req_t* req, UserAccount& out_user, bool even_with_default_password) {
    const std::string token = get_token(req);
    // the panel marks its own periodic refreshing, which must not keep an unattended session alive
    char poll[4];
    const bool refreshing = httpd_req_get_hdr_value_str(req, "X-Poll", poll, sizeof(poll)) != ESP_ERR_NOT_FOUND;
    if (token.empty() || !security_mgr_.validate_web_session(token, out_user, !refreshing)) return false;
    last_use_ = esp_timer_get_time() / 1000000 + 1; // an open panel asks for the logs every few seconds
    return even_with_default_password || !out_user.default_password;
}

bool WebServer::require_owner(httpd_req_t* req) {
    UserAccount user;
    if (g_web_server.get_authenticated_user(req, user) && get_role_level(user.global_role) >= get_role_level("owner")) {
        return true;
    }
    send_message(req, 403, "Owner login required.");
    return false;
}

static bool within_10s(uint32_t last) { return last != 0 && esp_timer_get_time() / 1000000 + 1 - last <= 10; }

bool WebServer::in_use() const { return within_10s(last_use_); }
bool WebServer::visited() const { return within_10s(last_visit_); }

bool WebServer::start(int port) {
    static const httpd_uri_t API[] = {
        {.uri = "/api/system", .method = HTTP_GET, .handler = api_system_handler, .user_ctx = nullptr},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler, .user_ctx = nullptr},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = api_wifi_scan_handler, .user_ctx = nullptr},
        {.uri = "/api/led", .method = HTTP_GET, .handler = api_led_get_handler, .user_ctx = nullptr},
        {.uri = "/api/led", .method = HTTP_POST, .handler = api_led_post_handler, .user_ctx = nullptr},
        {.uri = "/api/login", .method = HTTP_POST, .handler = api_login_handler, .user_ctx = nullptr},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = api_logout_handler, .user_ctx = nullptr},
        {.uri = "/api/me", .method = HTTP_GET, .handler = api_me_handler, .user_ctx = nullptr},
        {.uri = "/api/password", .method = HTTP_POST, .handler = api_password_handler, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler, .user_ctx = nullptr},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = api_logs_handler, .user_ctx = nullptr},
        {.uri = "/api/users", .method = HTTP_GET, .handler = api_users_get_handler, .user_ctx = nullptr},
        {.uri = "/api/users", .method = HTTP_POST, .handler = api_users_post_handler, .user_ctx = nullptr},
        {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler, .user_ctx = nullptr},
        {.uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler, .user_ctx = nullptr},
        {.uri = "/api/action", .method = HTTP_POST, .handler = api_action_handler, .user_ctx = nullptr},
        {.uri = "/api/backup", .method = HTTP_GET, .handler = api_backup_handler, .user_ctx = nullptr},
        {.uri = "/api/restore", .method = HTTP_POST, .handler = api_restore_handler, .user_ctx = nullptr},
        {.uri = "/api/ota/status", .method = HTTP_GET, .handler = api_ota_status_handler, .user_ctx = nullptr},
        {.uri = "/api/ota/upload", .method = HTTP_POST, .handler = api_ota_upload_handler, .user_ctx = nullptr},
        {.uri = "/api/ota/url", .method = HTTP_POST, .handler = api_ota_url_handler, .user_ctx = nullptr},
    };

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = std::size(API) + std::size(ASSETS);
    config.max_resp_headers = 8;
    config.stack_size = 12288; // console commands run on this task
    config.lru_purge_enable = true; // idle browser keep-alive connections must not lock others out

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server.");
        return false;
    }

    for (const auto& route : API) httpd_register_uri_handler(server_, &route);
    for (const auto& asset : ASSETS) {
        const httpd_uri_t route = {.uri = asset.path, .method = HTTP_GET, .handler = asset_handler, .user_ctx = const_cast<Asset*>(&asset)};
        httpd_register_uri_handler(server_, &route);
    }

    ESP_LOGI(TAG, "Web server started on port %d (home page and /panel)", port);
    return true;
}

void WebServer::stop() {
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}
