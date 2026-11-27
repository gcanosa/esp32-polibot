// Online updates: what the device knows about newer releases, a check, the upgrade and the automatic check
#include "web_server.h"
#include "bot_settings.h"
#include "config_manager.h"
#include "update_checker.h"

// GET /api/update
esp_err_t WebServer::api_update_get_handler(httpd_req_t* req) {
    if (!require_owner(req)) return ESP_OK;
    return send_json(req, g_updates.state_json(), 200);
}

// POST /api/update  {"action": "check" | "install"}  or  {"auto_check": bool, "check_hours": number}
esp_err_t WebServer::api_update_post_handler(httpd_req_t* req) {
    if (!require_owner(req)) return ESP_OK;
    cJSON* p = cJSON_Parse(read_post_body(req).c_str());
    const cJSON* action_i = cJSON_GetObjectItemCaseSensitive(p, "action");
    const std::string action = cJSON_IsString(action_i) ? action_i->valuestring : "";
    const cJSON* automatic = cJSON_GetObjectItemCaseSensitive(p, "auto_check");
    const cJSON* hours = cJSON_GetObjectItemCaseSensitive(p, "check_hours");
    int status = 200;
    std::string message;
    if (action == "check") {
        message = g_updates.check_now() ? "Checking..." : "A check is already running.";
    } else if (action == "install") {
        if (g_updates.install(message)) message = "Download started.";
        else status = 409;
    } else if (action.empty() && cJSON_IsBool(automatic) && cJSON_IsNumber(hours)) {
        // saved without a rehash, which would sign everybody out on IRC
        cJSON* wanted = cJSON_CreateObject();
        cJSON* section = cJSON_AddObjectToObject(wanted, "updates");
        cJSON_AddBoolToObject(section, "auto_check", cJSON_IsTrue(automatic));
        cJSON_AddNumberToObject(section, "check_hours", hours->valuedouble);
        sanitize_bot_settings(wanted);
        const bool saved = g_config_mgr.update([wanted](cJSON* root) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "updates");
            cJSON_AddItemToObject(root, "updates", cJSON_DetachItemFromObjectCaseSensitive(wanted, "updates"));
            return true;
        });
        cJSON_Delete(wanted);
        if (saved) g_settings.load();
        status = saved ? 200 : 500;
        message = saved ? "Update settings saved." : "Could not save to flash.";
    } else {
        status = 400;
        message = "Nothing to do.";
    }
    cJSON_Delete(p);
    return send_message(req, status, message);
}
