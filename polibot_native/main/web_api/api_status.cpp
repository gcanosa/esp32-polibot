// What the device and its connections are doing: status, logs, system figures, status LED
#include "web_server.h"
#include "ntp_manager.h"
#include "wifi_manager.h"
#include "system_info.h"
#include "status_led.h"
#include <esp_timer.h>
#include <esp_system.h>
#include "wifi_scan.h"
#include "update_checker.h"
#include <esp_app_desc.h>
#include <esp_random.h>
#include <cstdlib>

esp_err_t WebServer::api_system_handler(httpd_req_t* req) {
    // the status page says so while somebody has it on screen; the status LED shows it
    if (httpd_req_get_hdr_value_len(req, "X-Viewing")) g_web_server.last_visit_ = esp_timer_get_time() / 1000000 + 1;
    return send_json(req, system_info_json(), 200);
}

// GET and POST /api/led - colours and brightness of the status LED
esp_err_t WebServer::api_led_get_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user)) return send_message(req, 401, "Login required.");
    return send_json(req, status_led_settings_json(), 200);
}

esp_err_t WebServer::api_led_post_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user)) return send_message(req, 401, "Login required.");
    if (get_role_level(user.global_role) < get_role_level("admin")) return send_message(req, 403, "Administrator permission required.");
    cJSON* p = cJSON_Parse(read_post_body(req).c_str());
    if (!cJSON_IsObject(p)) {
        cJSON_Delete(p);
        return send_message(req, 400, "Invalid JSON");
    }
    const std::string problem = status_led_save(p);
    cJSON_Delete(p);
    return send_message(req, problem.empty() ? 200 : 400, problem.empty() ? "LED settings saved." : problem);
}

// POST /api/wifi/scan looks for networks nearby, GET gives what was found; for whoever may change the WiFi settings
esp_err_t WebServer::api_wifi_scan_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user)) return send_message(req, 401, "Login required.");
    if (get_role_level(user.global_role) < get_role_level("admin")) return send_message(req, 403, "Administrator permission required.");
    const char* problem = req->method == HTTP_POST ? wifi_scan_start() : "";
    cJSON* scan = wifi_scan_json();
    cJSON_AddStringToObject(scan, "problem", problem);
    return send_json(req, scan, 200);
}

// 5. GET /api/status
esp_err_t WebServer::api_status_handler(httpd_req_t* req) {
    // names servers, channels and nicknames: signed-in users only (the home page uses /api/system)
    UserAccount viewer;
    if (!g_web_server.get_authenticated_user(req, viewer)) return send_message(req, 401, "Login required.");
    int64_t now_sec = esp_timer_get_time() / 1000000;
    int64_t uptime_sec = now_sec - g_irc_manager.get_start_time();
    int days = uptime_sec / 86400;
    int hours = (uptime_sec % 86400) / 3600;
    int mins = (uptime_sec % 3600) / 60;
    int secs = uptime_sec % 60;
    char up_str[64];
    snprintf(up_str, sizeof(up_str), "%dd %02dh %02dm %02ds", days, hours, mins, secs);

    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(res, "update", g_updates.available().c_str()); // a newer version, "" if none is known
    cJSON_AddStringToObject(res, "uptime", up_str);
    cJSON_AddStringToObject(res, "current_time", g_ntp_mgr.get_formatted_time().c_str());
    cJSON_AddBoolToObject(res, "ntp_synced", g_ntp_mgr.is_synced());
    cJSON_AddStringToObject(res, "timezone", g_ntp_mgr.get_timezone().c_str());
    cJSON_AddStringToObject(res, "location", g_ntp_mgr.get_location().c_str());
    cJSON_AddStringToObject(res, "ntp_server", g_ntp_mgr.get_server().c_str());

    uint32_t free_ram = esp_get_free_heap_size() / 1024;
    cJSON_AddStringToObject(res, "free_ram", (std::to_string(free_ram) + " KB").c_str());

    cJSON_AddItemToObject(res, "led", status_led_state_json());

    cJSON* wifi = cJSON_AddObjectToObject(res, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", g_wifi_mgr.is_connected());
    cJSON_AddBoolToObject(wifi, "ap_active", g_wifi_mgr.ap_active());
    cJSON_AddStringToObject(wifi, "ip", g_wifi_mgr.get_ip().c_str());

    cJSON* net_arr = cJSON_AddArrayToObject(res, "networks");
    cJSON* ch_arr = cJSON_AddArrayToObject(res, "channels");
    cJSON* cd_arr = cJSON_AddArrayToObject(res, "channels_detail");
    bool any_connected = false;
    std::string first_host, first_nick;
    for (const auto& client : g_irc_manager.clients()) {
        const auto cfg = client->settings();
        if (first_host.empty()) {
            first_host = cfg->host;
            first_nick = client->current_nick();
        }
        any_connected = any_connected || client->is_connected();

        cJSON* net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "name", cfg->name.c_str());
        cJSON_AddStringToObject(net, "host", cfg->host.c_str());
        cJSON_AddNumberToObject(net, "port", cfg->port);
        cJSON_AddBoolToObject(net, "ssl", cfg->ssl);
        cJSON_AddStringToObject(net, "nick", client->current_nick().c_str());
        cJSON_AddBoolToObject(net, "connected", client->is_connected());
        cJSON_AddStringToObject(net, "report", client->report_status().c_str());
        cJSON_AddStringToObject(net, "bot_mode", client->bot_mode_status().c_str());
        cJSON_AddStringToObject(net, "accounts", client->account_status().c_str());
        cJSON_AddItemToArray(net_arr, net);

        for (const auto& ch : client->get_joined_channels()) {
            cJSON_AddItemToArray(ch_arr, cJSON_CreateString(ch.c_str()));
        }
        for (const auto& d : client->get_channels_detail()) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "network", d.network.c_str());
            cJSON_AddStringToObject(item, "name", d.name.c_str());
            cJSON_AddNumberToObject(item, "users", d.users);
            if (d.active_limit > 0) {
                cJSON_AddNumberToObject(item, "active_limit", d.active_limit);
            } else {
                cJSON_AddStringToObject(item, "active_limit", "N/A");
            }
            cJSON_AddBoolToObject(item, "dyn_enabled", d.dyn_enabled);
            cJSON_AddNumberToObject(item, "offset", d.offset);
            cJSON_AddBoolToObject(item, "entry_enabled", d.entry_enabled);
            cJSON_AddStringToObject(item, "entry_msg", d.entry_msg.c_str());
            cJSON_AddBoolToObject(item, "lockdown", d.lockdown);
            cJSON_AddNumberToObject(item, "op", d.op);
            cJSON_AddItemToArray(cd_arr, item);
        }
    }
    cJSON_AddBoolToObject(res, "irc_connected", any_connected);
    cJSON_AddStringToObject(res, "irc_server", first_host.c_str());
    cJSON_AddStringToObject(res, "nick", first_nick.c_str());

    return send_json(req, res, 200);
}

// 6. GET /api/logs gives every line held. With ?since=<number> only the newer ones, as
// {"boot", "oldest", "lines"}: the numbers start again when "boot" changes.
esp_err_t WebServer::api_logs_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user)) return send_message(req, 401, "Login required.");
    char query[32], value[12];
    const bool newer = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                       httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK;
    uint32_t oldest = 0;
    auto lines = g_log_buffer.get_lines(newer ? strtoul(value, nullptr, 10) : 0, &oldest);
    cJSON* arr = cJSON_CreateArray();
    for (const auto& l : lines) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", l.id);
        cJSON_AddNumberToObject(item, "time", l.timestamp);
        cJSON_AddStringToObject(item, "msg", l.message.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    if (!newer) return send_json(req, arr, 200);
    static const uint32_t boot = esp_random() | 1;
    cJSON* res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "boot", boot);
    cJSON_AddNumberToObject(res, "oldest", oldest);
    cJSON_AddItemToObject(res, "lines", arr);
    return send_json(req, res, 200);
}
