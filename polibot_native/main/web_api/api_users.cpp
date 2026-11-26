// User accounts
#include "web_server.h"
#include <strings.h>

// 7. GET /api/users
esp_err_t WebServer::api_users_get_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user) || get_role_level(user.global_role) < get_role_level("admin")) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "forbidden");
        return send_json(req, err, 403);
    }

    auto users = g_security_mgr.list_users();
    cJSON* arr = cJSON_CreateArray();
    for (const auto& u : users) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "username", u.username.c_str());
        cJSON_AddStringToObject(item, "global_role", u.global_role.c_str());
        cJSON* cr = cJSON_CreateObject();
        for (const auto& pair : u.channel_roles) {
            cJSON_AddStringToObject(cr, pair.first.c_str(), pair.second.c_str());
        }
        cJSON_AddItemToObject(item, "channel_roles", cr);

        cJSON* masks = cJSON_CreateArray();
        for (const auto& m : u.hostmasks) {
            cJSON_AddItemToArray(masks, cJSON_CreateString(m.c_str()));
        }
        cJSON_AddItemToObject(item, "hostmasks", masks);
        cJSON* accounts = cJSON_AddArrayToObject(item, "accounts");
        for (const auto& a : u.accounts) cJSON_AddItemToArray(accounts, cJSON_CreateString(a.c_str()));
        cJSON_AddItemToArray(arr, item);
    }
    return send_json(req, arr, 200);
}

// 8. POST /api/users
esp_err_t WebServer::api_users_post_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user) || get_role_level(user.global_role) < get_role_level("admin")) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "forbidden");
        return send_json(req, err, 403);
    }

    std::string body = read_post_body(req);
    cJSON* p = cJSON_Parse(body.c_str());
    if (!p) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Invalid JSON");
        return send_json(req, err, 400);
    }

    cJSON* act_i = cJSON_GetObjectItemCaseSensitive(p, "action");
    cJSON* u_i = cJSON_GetObjectItemCaseSensitive(p, "username");
    std::string action = (act_i && act_i->valuestring) ? act_i->valuestring : "save";
    std::string target_user = (u_i && u_i->valuestring) ? u_i->valuestring : "";

    if (get_role_level(user.global_role) < get_role_level("owner")) {
        cJSON* role_i = cJSON_GetObjectItemCaseSensitive(p, "global_role");
        cJSON* ch_i = cJSON_GetObjectItemCaseSensitive(p, "channel_roles");
        bool touches_owner = role_i && role_i->valuestring && get_role_level(role_i->valuestring) >= get_role_level("owner");
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, ch_i) {
            if (it->valuestring && get_role_level(it->valuestring) >= get_role_level("owner")) touches_owner = true;
        }
        for (const auto& u : g_security_mgr.list_users()) {
            if (strcasecmp(u.username.c_str(), target_user.c_str()) == 0 &&
                get_role_level(u.global_role) >= get_role_level("owner")) touches_owner = true;
        }
        if (touches_owner) {
            cJSON_Delete(p);
            return send_message(req, 403, "Only an owner can create or modify owner accounts.");
        }
    }

    if (action == "delete") {
        std::string out_msg;
        bool ok = g_security_mgr.remove_user(target_user, out_msg);
        cJSON_Delete(p);
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "status", ok ? "ok" : "error");
        cJSON_AddStringToObject(res, "message", out_msg.c_str());
        return send_json(req, res, ok ? 200 : 400);
    } else {
        cJSON* pw_i = cJSON_GetObjectItemCaseSensitive(p, "password");
        cJSON* g_i = cJSON_GetObjectItemCaseSensitive(p, "global_role");
        cJSON* ch_roles = cJSON_GetObjectItemCaseSensitive(p, "channel_roles");
        cJSON* masks_i = cJSON_GetObjectItemCaseSensitive(p, "hostmasks");

        std::string pw = (pw_i && pw_i->valuestring) ? pw_i->valuestring : "";
        std::string g_role = (g_i && g_i->valuestring) ? g_i->valuestring : "";
        std::map<std::string, std::string> ch_map;
        if (cJSON_IsObject(ch_roles)) {
            cJSON* it = nullptr;
            cJSON_ArrayForEach(it, ch_roles) {
                if (it->string && it->valuestring) ch_map[it->string] = it->valuestring;
            }
        }
        std::vector<std::string> masks;
        if (cJSON_IsArray(masks_i)) {
            int sz = cJSON_GetArraySize(masks_i);
            for (int i = 0; i < sz; i++) {
                cJSON* m = cJSON_GetArrayItem(masks_i, i);
                if (m && m->valuestring) masks.push_back(m->valuestring);
            }
        }

        // channel roles and services accounts: left alone when not sent, replaced (also by none) when sent
        cJSON* accounts_i = cJSON_GetObjectItemCaseSensitive(p, "accounts");
        std::vector<std::string> accounts;
        cJSON* a = nullptr;
        cJSON_ArrayForEach(a, accounts_i) {
            if (cJSON_IsString(a)) accounts.push_back(a->valuestring);
        }

        auto result = g_security_mgr.add_or_update_user(target_user, pw, g_role, cJSON_IsObject(ch_roles) ? &ch_map : nullptr, masks, get_token(req),
                                                        cJSON_IsArray(accounts_i) ? &accounts : nullptr);
        cJSON_Delete(p);
        return send_message(req, result.first ? 200 : 400, result.second);
    }
}
