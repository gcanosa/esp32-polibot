#pragma once

#include <atomic>
#include <esp_http_server.h>
#include "config_manager.h"
#include "security_manager.h"
#include "irc_client.h"

constexpr int MAX_CONFIG_BODY = 96 * 1024; // a full configuration or backup

// The panel's server. Its source files, by subject: web_server (routes, pages, shared helpers),
// api_session, api_status, api_users, api_config, api_action, api_access, api_backup, api_ota, api_update.
class WebServer {
public:
    WebServer(ConfigManager& config_mgr, SecurityManager& security_mgr, IRCManager& irc_mgr);
    ~WebServer();

    bool start(int port = 80);
    void stop();
    bool in_use() const;  // a signed-in panel made a request in the last 10 seconds
    bool visited() const; // the status page was on somebody's screen in the last 10 seconds

private:
    ConfigManager& config_mgr_;
    SecurityManager& security_mgr_;
    IRCManager& irc_mgr_;
    httpd_handle_t server_ = nullptr;
    std::atomic<uint32_t> last_use_{0};   // seconds since boot, plus one; 0 = never
    std::atomic<uint32_t> last_visit_{0}; // the same, for the status page

    // False for an account still on the default password, except where it is on its way to change it
    bool get_authenticated_user(httpd_req_t* req, UserAccount& out_user, bool even_with_default_password = false);
    static esp_err_t send_json(httpd_req_t* req, cJSON* json, int status_code = 200);
    static std::string read_post_body(httpd_req_t* req, int limit = 32768);
    static std::string get_token(httpd_req_t* req);
    static bool require_owner(httpd_req_t* req);
    static esp_err_t send_message(httpd_req_t* req, int status_code, const std::string& message);
    // "access_add" and "access_del" of /api/action, once the caller's right to them is checked
    static esp_err_t access_action(httpd_req_t* req, const cJSON* body, IRCClient& client, const std::string& channel,
                                   const std::string& user, bool adding);

    // Handlers
    static esp_err_t api_system_handler(httpd_req_t* req);
    static esp_err_t api_led_get_handler(httpd_req_t* req);
    static esp_err_t api_led_post_handler(httpd_req_t* req);
    static esp_err_t api_wifi_scan_handler(httpd_req_t* req);
    static esp_err_t api_login_handler(httpd_req_t* req);
    static esp_err_t api_logout_handler(httpd_req_t* req);
    static esp_err_t api_me_handler(httpd_req_t* req);
    static esp_err_t api_status_handler(httpd_req_t* req);
    static esp_err_t api_logs_handler(httpd_req_t* req);
    static esp_err_t api_users_get_handler(httpd_req_t* req);
    static esp_err_t api_users_post_handler(httpd_req_t* req);
    static esp_err_t api_password_handler(httpd_req_t* req);
    static esp_err_t api_config_get_handler(httpd_req_t* req);
    static esp_err_t api_config_post_handler(httpd_req_t* req);
    static esp_err_t api_action_handler(httpd_req_t* req);
    static esp_err_t api_ota_status_handler(httpd_req_t* req);
    static esp_err_t api_ota_upload_handler(httpd_req_t* req);
    static esp_err_t api_ota_url_handler(httpd_req_t* req);
    static esp_err_t api_update_get_handler(httpd_req_t* req);
    static esp_err_t api_update_post_handler(httpd_req_t* req);
    static esp_err_t api_backup_handler(httpd_req_t* req);
    static esp_err_t api_restore_handler(httpd_req_t* req);
};

extern WebServer g_web_server;
