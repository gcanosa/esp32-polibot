// Reading and saving the configuration
#include "web_server.h"
#include "config_checks.h"
#include <cstring>
#include <strings.h>
#include <vector>

// Passwords are write-only through this API: reading shows the mask, saving the mask keeps the stored value
static const char* SECRET_MASK = "********";

static void mask_secret(cJSON* obj, const cJSON*, const char* key) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring[0]) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, cJSON_CreateString(SECRET_MASK));
}

static void unmask_secret(cJSON* obj, const cJSON* stored, const char* key) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || strcmp(v->valuestring, SECRET_MASK) != 0) return;
    cJSON* old = cJSON_GetObjectItemCaseSensitive(stored, key);
    cJSON_ReplaceItemInObjectCaseSensitive(obj, key, cJSON_CreateString(cJSON_IsString(old) ? old->valuestring : ""));
}

// Applies `apply` to every secret in `config`, pairing each with the same place in `stored` (networks by name)
static void for_each_secret(cJSON* config, const cJSON* stored, void (*apply)(cJSON*, const cJSON*, const char*)) {
    cJSON* wifi = cJSON_GetObjectItemCaseSensitive(config, "wifi");
    const cJSON* old_wifi = cJSON_GetObjectItemCaseSensitive(stored, "wifi");
    apply(wifi, old_wifi, "password");
    apply(wifi, old_wifi, "ap_password");

    cJSON* irc = cJSON_GetObjectItemCaseSensitive(config, "irc");
    const cJSON* old_irc = cJSON_GetObjectItemCaseSensitive(stored, "irc");
    apply(irc, old_irc, "nickserv_password"); // single-network key, kept for rollback
    cJSON* net = nullptr;
    cJSON_ArrayForEach(net, cJSON_GetObjectItemCaseSensitive(irc, "networks")) {
        cJSON* name = cJSON_GetObjectItemCaseSensitive(net, "name");
        const cJSON* match = nullptr;
        cJSON* old = nullptr;
        cJSON_ArrayForEach(old, cJSON_GetObjectItemCaseSensitive(old_irc, "networks")) {
            cJSON* old_name = cJSON_GetObjectItemCaseSensitive(old, "name");
            if (cJSON_IsString(name) && cJSON_IsString(old_name) && strcasecmp(name->valuestring, old_name->valuestring) == 0) match = old;
        }
        apply(net, match, "nickserv_password");
        apply(net, match, "report_key");

        // passwords of guarded channels; the names are collected first because `apply` replaces entries
        cJSON* keys = cJSON_GetObjectItemCaseSensitive(net, "channel_keys");
        const cJSON* old_keys = cJSON_GetObjectItemCaseSensitive(match, "channel_keys");
        std::vector<std::string> channels;
        cJSON* key = nullptr;
        cJSON_ArrayForEach(key, keys) channels.push_back(key->string);
        for (const auto& channel : channels) apply(keys, old_keys, channel.c_str());
    }
}

// Server dialect settings a network may leave out, so the panel shows what is in effect
static void fill_network_defaults(cJSON* root) {
    const IrcSettings defaults;
    cJSON* net = nullptr;
    cJSON_ArrayForEach(net, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "irc"), "networks")) {
        auto fill = [&](const char* key, const std::string& value) {
            if (!cJSON_GetObjectItemCaseSensitive(net, key)) cJSON_AddStringToObject(net, key, value.c_str());
        };
        fill("quiet_mode", defaults.quiet_mode);
        fill("ops_command", defaults.ops_command);
        fill("invite_command", defaults.invite_command);
        fill("unban_command", defaults.unban_command);
    }
}

// 9. GET /api/config
esp_err_t WebServer::api_config_get_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user)) return send_message(req, 401, "Login required.");

    cJSON* root = g_config_mgr.get_root();
    if (!root) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        return send_json(req, err, 500);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "users"); // see /api/users (administrators)
    cJSON_DeleteItemFromObjectCaseSensitive(root, "web");
    for_each_secret(root, nullptr, mask_secret);
    fill_bot_settings(root);
    fill_network_defaults(root);
    return send_json(req, root, 200);
}

// 10. POST /api/config
esp_err_t WebServer::api_config_post_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user) || get_role_level(user.global_role) < get_role_level("admin")) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "forbidden");
        return send_json(req, err, 403);
    }

    std::string body = read_post_body(req, MAX_CONFIG_BODY);
    cJSON* p = cJSON_Parse(body.c_str());
    if (!p) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        return send_json(req, err, 400);
    }

    cJSON* stored = g_config_mgr.get_root();
    for_each_secret(p, stored, unmask_secret); // first: validation looks at the real values
    cJSON_Delete(stored);

    cJSON* irc_i = cJSON_GetObjectItemCaseSensitive(p, "irc");
    std::string invalid = irc_i ? validate_irc_config(irc_i) : "";
    cJSON* wifi_i = cJSON_GetObjectItemCaseSensitive(p, "wifi");
    if (invalid.empty() && wifi_i) invalid = validate_wifi_config(wifi_i);
    cJSON* guardian_i = cJSON_GetObjectItemCaseSensitive(p, "guardian");
    if (invalid.empty() && guardian_i) invalid = validate_guardian_config(guardian_i);
    if (!invalid.empty()) {
        cJSON_Delete(p);
        return send_message(req, 400, invalid);
    }
    sanitize_bot_settings(p);

    std::string changed;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, p) {
        if (it->string && strcmp(it->string, "users") != 0) {
            changed += (changed.empty() ? "" : ", ") + std::string(it->string);
            g_config_mgr.set_item(it->string, cJSON_Duplicate(it, 1));
        }
    }
    cJSON_Delete(p);
    if (!g_config_mgr.save()) return send_message(req, 500, "Could not save to flash.");
    g_config_mgr.rehash();
    g_irc_manager.report(ReportKind::system, user.username + " changed settings in the web panel: " + changed);

    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", "ok");
    cJSON_AddStringToObject(res, "message", "Configuration updated and rehashed.");
    return send_json(req, res, 200);
}
