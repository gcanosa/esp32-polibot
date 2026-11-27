// Signing in and out, and changing one's own password
#include "web_server.h"
#include <cstring>

// 2. POST /api/login
esp_err_t WebServer::api_login_handler(httpd_req_t* req) {
    std::string body = read_post_body(req);
    cJSON* p = cJSON_Parse(body.c_str());
    if (!p) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Invalid JSON");
        return send_json(req, err, 400);
    }

    cJSON* u_item = cJSON_GetObjectItemCaseSensitive(p, "username");
    cJSON* pw_item = cJSON_GetObjectItemCaseSensitive(p, "password");
    std::string uname = (u_item && u_item->valuestring) ? u_item->valuestring : "";
    std::string pw = (pw_item && pw_item->valuestring) ? pw_item->valuestring : "";
    cJSON_Delete(p);

    const std::string address = client_address(req);
    if (const int wait = g_security_mgr.login_blocked_for("web " + address)) {
        return send_message(req, 429, "Too many failed sign-ins. Try again in " + format_duration(wait) + ".");
    }

    UserAccount user;
    if (g_security_mgr.authenticate(uname, pw, user)) {
        std::string token = g_security_mgr.create_web_session(user.username, address);
        g_irc_manager.report(ReportKind::security, "Web panel: " + user.username + " (" + user.global_role + ") signed in from " + address + ".");
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "status", "ok");
        cJSON_AddStringToObject(res, "token", token.c_str());
        // until then this session can do nothing but change the password
        cJSON_AddBoolToObject(res, "must_change_password", user.default_password);

        cJSON* u_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(u_obj, "username", user.username.c_str());
        cJSON_AddStringToObject(u_obj, "global_role", user.global_role.c_str());
        cJSON* cr = cJSON_CreateObject();
        for (const auto& pair : user.channel_roles) {
            cJSON_AddStringToObject(cr, pair.first.c_str(), pair.second.c_str());
        }
        cJSON_AddItemToObject(u_obj, "channel_roles", cr);
        cJSON_AddItemToObject(res, "user", u_obj);

        return send_json(req, res, 200);
    } else {
        const bool paused = g_security_mgr.login_failed("web " + address);
        g_irc_manager.report(ReportKind::security, "Web panel: failed sign-in as '" + uname.substr(0, 32) + "' from " + address +
                                                       (paused ? ". Sign-ins from there are paused." : "."));
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Invalid username or password");
        return send_json(req, err, 401);
    }
}

// 3. POST /api/logout
esp_err_t WebServer::api_logout_handler(httpd_req_t* req) {
    char auth_hdr[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) == ESP_OK) {
        std::string h = auth_hdr;
        if (h.rfind("Bearer ", 0) == 0) {
            g_security_mgr.destroy_web_session(h.substr(7));
        }
    }
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", "ok");
    return send_json(req, res, 200);
}

// 4. GET /api/me
esp_err_t WebServer::api_me_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user, true)) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "unauthorized");
        return send_json(req, err, 401);
    }
    cJSON* res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "must_change_password", user.default_password);
    cJSON_AddStringToObject(res, "username", user.username.c_str());
    cJSON_AddStringToObject(res, "global_role", user.global_role.c_str());
    cJSON* cr = cJSON_CreateObject();
    for (const auto& pair : user.channel_roles) {
        cJSON_AddStringToObject(cr, pair.first.c_str(), pair.second.c_str());
    }
    cJSON_AddItemToObject(res, "channel_roles", cr);
    // an administrator behind a proxy the device does not know is offered to trust it
    if (!user.default_password && get_role_level(user.global_role) >= get_role_level("admin")) {
        const std::string proxy = untrusted_proxy(req);
        if (!proxy.empty()) cJSON_AddStringToObject(res, "proxy", proxy.c_str());
    }
    return send_json(req, res, 200);
}

// POST /api/password {"current": "...", "password": "..."} - anyone signed in changes their own
esp_err_t WebServer::api_password_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user, true)) return send_message(req, 401, "Login required.");

    cJSON* p = cJSON_Parse(read_post_body(req).c_str());
    cJSON* cur_i = cJSON_GetObjectItemCaseSensitive(p, "current");
    cJSON* new_i = cJSON_GetObjectItemCaseSensitive(p, "password");
    const std::string current = cJSON_IsString(cur_i) ? cur_i->valuestring : "";
    const std::string next = cJSON_IsString(new_i) ? new_i->valuestring : "";
    cJSON_Delete(p);

    UserAccount checked;
    if (!g_security_mgr.authenticate(user.username, current, checked)) return send_message(req, 403, "The current password is wrong.");
    auto result = g_security_mgr.set_password(user.username, next, get_token(req));
    return send_message(req, result.first ? 200 : 400, result.second);
}
