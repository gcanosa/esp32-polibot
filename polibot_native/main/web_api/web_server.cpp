// The web server itself: routes, the embedded pages and what every API handler shares
#include "web_server.h"
#include "health.h"
#include "proxy.h"
#include <lwip/sockets.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iterator>

static const char* TAG = "WEB";

// Files compiled into the firmware (main/web), sent straight from flash. The text ones are packed
// with gzip while building (main/CMakeLists.txt) and sent that way.
#define EMBEDDED(symbol)                                                    \
    extern const char symbol##_start[] asm("_binary_" #symbol "_start"); \
    extern const char symbol##_end[] asm("_binary_" #symbol "_end")
EMBEDDED(index_html_gz);
EMBEDDED(home_html_gz);
EMBEDDED(logo_webp);
EMBEDDED(board_webp);
EMBEDDED(theme_js_gz);
EMBEDDED(ui_css_gz);
EMBEDDED(panel_css_gz);
EMBEDDED(panel_core_js_gz);
EMBEDDED(panel_irc_js_gz);
EMBEDDED(panel_rules_js_gz);
EMBEDDED(panel_users_js_gz);
EMBEDDED(panel_system_js_gz);
EMBEDDED(panel_logs_js_gz);
EMBEDDED(panel_wifi_js_gz);
EMBEDDED(panel_access_js_gz);
EMBEDDED(panel_update_js_gz);
EMBEDDED(panel_channel_js_gz);
EMBEDDED(panel_sessions_js_gz);

namespace {

struct Asset {
    const char* path;
    const char* type;
    const char* start;
    const char* end;
    bool text; // gzipped, and tied to this firmware; pictures are neither
};

char s_version_tag[24]; // names this firmware, so a browser asks again only after an update

const char* HTML = "text/html; charset=utf-8";
const char* JS = "text/javascript; charset=utf-8";
const char* CSS = "text/css; charset=utf-8";

const Asset ASSETS[] = {
    {"/", HTML, home_html_gz_start, home_html_gz_end, true},
    {"/index.html", HTML, home_html_gz_start, home_html_gz_end, true},
    {"/panel", HTML, index_html_gz_start, index_html_gz_end, true},
    {"/ui.css", CSS, ui_css_gz_start, ui_css_gz_end, true},
    {"/panel.css", CSS, panel_css_gz_start, panel_css_gz_end, true},
    {"/panel_core.js", JS, panel_core_js_gz_start, panel_core_js_gz_end, true},
    {"/panel_irc.js", JS, panel_irc_js_gz_start, panel_irc_js_gz_end, true},
    {"/panel_rules.js", JS, panel_rules_js_gz_start, panel_rules_js_gz_end, true},
    {"/panel_users.js", JS, panel_users_js_gz_start, panel_users_js_gz_end, true},
    {"/panel_system.js", JS, panel_system_js_gz_start, panel_system_js_gz_end, true},
    {"/panel_logs.js", JS, panel_logs_js_gz_start, panel_logs_js_gz_end, true},
    {"/panel_wifi.js", JS, panel_wifi_js_gz_start, panel_wifi_js_gz_end, true},
    {"/panel_access.js", JS, panel_access_js_gz_start, panel_access_js_gz_end, true},
    {"/panel_update.js", JS, panel_update_js_gz_start, panel_update_js_gz_end, true},
    {"/panel_channel.js", JS, panel_channel_js_gz_start, panel_channel_js_gz_end, true},
    {"/panel_sessions.js", JS, panel_sessions_js_gz_start, panel_sessions_js_gz_end, true},
    {"/theme.js", JS, theme_js_gz_start, theme_js_gz_end, true},
    {"/logo.webp", "image/webp", logo_webp_start, logo_webp_end, false},
    {"/board.webp", "image/webp", board_webp_start, board_webp_end, false},
};

esp_err_t asset_handler(httpd_req_t* req) {
    const Asset& asset = *static_cast<const Asset*>(req->user_ctx);
    httpd_resp_set_type(req, asset.type);
    // pages and scripts must match the firmware that serves them; the logo hardly changes
    httpd_resp_set_hdr(req, "Cache-Control", asset.text ? "no-cache" : "public, max-age=86400");
    httpd_resp_set_hdr(req, "ETag", s_version_tag);
    char known[64];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", known, sizeof(known)) == ESP_OK && strstr(known, s_version_tag)) {
        httpd_resp_set_status(req, "304 Not Modified"); // the browser's copy is this one
        return httpd_resp_send(req, nullptr, 0);
    }
    if (asset.text) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, asset.start, asset.end - asset.start);
}

std::atomic<int> s_watch{-1}; // the web server's place in the hang check

// Bytes moving on a connection show that the server works, however long a request takes: an upload, a
// slow browser, or headers sent a byte at a time (which must not be a way to restart the device).
// Errors are reported as the server's own functions would.
int moved(int n) {
    if (n < 0) return errno == EAGAIN || errno == EINTR ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
    watch_beat(s_watch);
    return n;
}

int watched_recv(httpd_handle_t, int fd, char* buf, size_t length, int flags) { return moved(recv(fd, buf, length, flags)); }
int watched_send(httpd_handle_t, int fd, const char* buf, size_t length, int flags) { return moved(send(fd, buf, length, flags)); }

esp_err_t watch_connection(httpd_handle_t server, int fd) {
    httpd_sess_set_recv_override(server, fd, watched_recv);
    httpd_sess_set_send_override(server, fd, watched_send);
    return ESP_OK;
}

void still_serving(void*) { watch_beat(s_watch); }

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

// The address at the other end of the connection
static std::string peer_address(httpd_req_t* req) {
    sockaddr_in6 peer = {};
    socklen_t length = sizeof(peer);
    char text[INET6_ADDRSTRLEN] = "unknown";
    if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr*>(&peer), &length) == 0) {
        if (peer.sin6_family == AF_INET6) inet_ntop(AF_INET6, &peer.sin6_addr, text, sizeof(text));
        else inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&peer)->sin_addr, text, sizeof(text));
    }
    const std::string address = text;
    return address.rfind("::FFFF:", 0) == 0 || address.rfind("::ffff:", 0) == 0 ? address.substr(7) : address;
}

static std::string forwarded_for(httpd_req_t* req) {
    // read whole (the server limits the headers): the address that counts is at the end
    const size_t length = httpd_req_get_hdr_value_len(req, "X-Forwarded-For");
    if (!length) return "";
    std::string header(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "X-Forwarded-For", &header[0], header.size()) != ESP_OK) return "";
    header.resize(length);
    return forwarded_address(header);
}

std::string WebServer::client_address(httpd_req_t* req) {
    const std::string peer = peer_address(req);
    if (!proxy_listed(g_settings.get()->trusted_proxies, peer)) return peer;
    const std::string visitor = forwarded_for(req);
    return visitor.empty() ? peer : visitor;
}

std::string WebServer::untrusted_proxy(httpd_req_t* req) {
    const std::string peer = peer_address(req);
    if (forwarded_for(req).empty() || !valid_proxy_list(peer) || proxy_listed(g_settings.get()->trusted_proxies, peer)) return "";
    return peer;
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
        {.uri = "/api/update", .method = HTTP_GET, .handler = api_update_get_handler, .user_ctx = nullptr},
        {.uri = "/api/update", .method = HTTP_POST, .handler = api_update_post_handler, .user_ctx = nullptr},
        {.uri = "/api/sessions", .method = HTTP_GET, .handler = api_sessions_get_handler, .user_ctx = nullptr},
        {.uri = "/api/sessions", .method = HTTP_POST, .handler = api_sessions_post_handler, .user_ctx = nullptr},
    };

    char hash[17];
    esp_app_get_elf_sha256(hash, sizeof(hash));
    snprintf(s_version_tag, sizeof(s_version_tag), "\"%s\"", hash);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = std::size(API) + std::size(ASSETS);
    config.max_resp_headers = 8;
    config.stack_size = 12288; // console commands run on this task
    config.lru_purge_enable = true; // idle browser keep-alive connections must not lock others out
    config.open_fn = watch_connection;

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

void WebServer::check_alive() {
    if (!server_) return;
    if (s_watch < 0) s_watch = watch_start("The web panel");
    httpd_queue_work(server_, still_serving, nullptr); // run by the server once it is free
}

void WebServer::stop() {
    if (server_) {
        watch_stop(s_watch.exchange(-1));
        httpd_stop(server_);
        server_ = nullptr;
    }
}
