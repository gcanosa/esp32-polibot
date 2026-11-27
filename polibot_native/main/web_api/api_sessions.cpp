// Who is signed in, on IRC and in the panel, and signing them out (administrators)
#include "web_server.h"

esp_err_t WebServer::api_sessions_get_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user) || get_role_level(user.global_role) < get_role_level("admin")) {
        return send_message(req, 403, "Administrator permission required.");
    }
    cJSON* arr = cJSON_CreateArray();
    for (const auto& s : g_security_mgr.sessions(get_token(req))) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", s.id.c_str());
        cJSON_AddStringToObject(item, "user", s.user.c_str());
        cJSON_AddStringToObject(item, "network", s.network.c_str());
        cJSON_AddStringToObject(item, "nick", s.nick.c_str());
        cJSON_AddStringToObject(item, "from", s.from.c_str());
        cJSON_AddNumberToObject(item, "idle", s.idle);
        cJSON_AddBoolToObject(item, "current", s.current);
        cJSON_AddItemToArray(arr, item);
    }
    return send_json(req, arr, 200);
}

// {"id": "..."} as listed
esp_err_t WebServer::api_sessions_post_handler(httpd_req_t* req) {
    UserAccount user;
    if (!g_web_server.get_authenticated_user(req, user) || get_role_level(user.global_role) < get_role_level("admin")) {
        return send_message(req, 403, "Administrator permission required.");
    }
    cJSON* p = cJSON_Parse(read_post_body(req).c_str());
    cJSON* id_i = cJSON_GetObjectItemCaseSensitive(p, "id");
    const std::string id = cJSON_IsString(id_i) ? id_i->valuestring : "";
    cJSON_Delete(p);
    std::string whom;
    const std::string problem = g_security_mgr.end_session(id, get_role_level(user.global_role) >= get_role_level("owner"), whom);
    if (!problem.empty()) return send_message(req, 400, problem);
    g_irc_manager.report(ReportKind::security, user.username + " (web panel) signed " + whom + " out.");
    return send_message(req, 200, "Signed " + whom + " out.");
}
