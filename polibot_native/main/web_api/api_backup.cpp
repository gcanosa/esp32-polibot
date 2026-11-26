// Settings backup and restore, owner only: the file holds every password
#include "web_server.h"
#include "config_checks.h"
#include "ntp_manager.h"
#include <esp_system.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

esp_err_t WebServer::api_backup_handler(httpd_req_t* req) {
    if (!require_owner(req)) return ESP_OK;
    cJSON* config = g_config_mgr.get_root();
    if (!config) return send_message(req, 500, "No configuration to save.");
    cJSON* res = cJSON_CreateObject();
    cJSON_AddNumberToObject(res, "polibot_backup", 1);
    cJSON_AddStringToObject(res, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(res, "created", g_ntp_mgr.get_formatted_time().c_str());
    cJSON_AddItemToObject(res, "config", config);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return send_json(req, res, 200);
}

// Empty if this configuration can be run, and signed in to, otherwise the reason it cannot
static std::string validate_restore(const cJSON* config) {
    cJSON* irc = cJSON_GetObjectItemCaseSensitive(config, "irc");
    if (!cJSON_IsObject(irc)) return "The backup has no IRC settings.";
    std::string problem = validate_irc_config(irc);
    cJSON* wifi = cJSON_GetObjectItemCaseSensitive(config, "wifi");
    if (problem.empty() && wifi) problem = validate_wifi_config(wifi);
    cJSON* guardian = cJSON_GetObjectItemCaseSensitive(config, "guardian");
    if (problem.empty() && guardian) problem = validate_guardian_config(guardian);
    if (!problem.empty()) return problem;
    cJSON* user = nullptr;
    cJSON_ArrayForEach(user, cJSON_GetObjectItemCaseSensitive(config, "users")) {
        cJSON* role = cJSON_GetObjectItemCaseSensitive(user, "global_role");
        if (cJSON_IsString(role) && get_role_level(role->valuestring) >= get_role_level("owner") &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(user, "username")) &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(user, "salt")) &&
            cJSON_IsString(cJSON_GetObjectItemCaseSensitive(user, "password_hash"))) return "";
    }
    return "The backup has no owner account, so nobody could sign in.";
}

// POST /api/restore {"backup": <a downloaded file>, "keep_wifi": true}
esp_err_t WebServer::api_restore_handler(httpd_req_t* req) {
    if (!require_owner(req)) return ESP_OK;
    cJSON* p = cJSON_Parse(read_post_body(req, MAX_CONFIG_BODY).c_str());
    cJSON* backup = cJSON_GetObjectItemCaseSensitive(p, "backup");
    cJSON* marker = cJSON_GetObjectItemCaseSensitive(backup, "polibot_backup");
    if (!cJSON_IsNumber(marker) || marker->valueint != 1 || !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(backup, "config"))) {
        cJSON_Delete(p);
        return send_message(req, 400, "This is not a PoliBOT settings backup (or it is too large).");
    }
    const bool keep_wifi = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "keep_wifi"));
    cJSON* config = cJSON_DetachItemFromObjectCaseSensitive(backup, "config");
    cJSON_Delete(p);
    if (keep_wifi) {
        cJSON* current = g_config_mgr.get_item("wifi");
        if (current) {
            cJSON_DeleteItemFromObjectCaseSensitive(config, "wifi");
            cJSON_AddItemToObject(config, "wifi", current);
        }
    }
    const std::string problem = validate_restore(config);
    if (!problem.empty()) {
        cJSON_Delete(config);
        return send_message(req, 400, problem);
    }
    if (!g_config_mgr.replace_root(config)) return send_message(req, 500, "Could not save to flash. Nothing was changed.");
    g_log_buffer.log("Settings restored from a backup. Restarting...");
    send_message(req, 200, "Settings restored. Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1500)); // let the response leave the socket
    esp_restart();
    return ESP_OK;
}
