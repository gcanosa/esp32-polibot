// The auto-op, half-op and voice lists of one channel
#include "web_server.h"
#include "irc_masks.h"

// {"action": "access_add" | "access_del", "network", "channel", "mode": "o" | "h" | "v", "who"}
esp_err_t WebServer::access_action(httpd_req_t* req, const cJSON* body, IRCClient& client, const std::string& channel,
                                   const std::string& user, bool adding) {
    cJSON* mode_i = cJSON_GetObjectItemCaseSensitive(body, "mode");
    cJSON* who_i = cJSON_GetObjectItemCaseSensitive(body, "who");
    const std::string mode = cJSON_IsString(mode_i) ? mode_i->valuestring : "";
    const std::string typed = cJSON_IsString(who_i) ? who_i->valuestring : "";
    if (mode.size() != 1 || !is_channel_name(channel) || typed.empty()) return send_message(req, 400, "Name a channel, a list and who.");
    // adding stores what the typed text stands for; removing names an entry exactly
    const std::string who = adding ? client.access_who(typed) : typed;
    if (adding) {
        const std::string problem = g_guardian.add_access(client.name(), channel, mode[0], who);
        if (!problem.empty()) return send_message(req, 400, problem);
    } else if (!g_guardian.remove_access(client.name(), channel, mode[0], who)) {
        return send_message(req, 400, "That entry is not on the list.");
    }
    client.review_modes(channel);
    const std::string done = (adding ? "Added " : "Removed ") + who + (adding ? " to" : " from") + " the +" + mode + " list of " + channel + ".";
    client.report(ReportKind::moderation, user + " (web panel): " + done);
    return send_message(req, 200, done);
}
