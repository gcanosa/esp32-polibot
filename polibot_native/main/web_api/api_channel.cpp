// Who is in a channel, what the bot has banned or muted there, and lifting it
#include "web_server.h"
#include "bot_memory.h"
#include <algorithm>

// {"action": "channel_view", "network", "channel"}, asked only while the panel shows a channel
esp_err_t WebServer::channel_view(httpd_req_t* req, IRCClient& client, const std::string& channel) {
    PsramVector<Member> members;
    int op = -1;
    std::string topic;
    const bool joined = client.channel_snapshot(channel, members, op, topic);

    cJSON* res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "joined", joined);
    cJSON_AddNumberToObject(res, "op", op);
    cJSON_AddStringToObject(res, "topic", topic.c_str());
    cJSON* list = cJSON_AddArrayToObject(res, "members");
    for (const auto& m : members) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "nick", m.nick);
        cJSON_AddStringToObject(item, "modes", m.modes);
        cJSON_AddStringToObject(item, "host", m.userhost);
        cJSON_AddItemToArray(list, item);
    }
    cJSON* bans = cJSON_AddArrayToObject(res, "bans");
    const Clock now = bot_clock();
    for (const auto& r : g_memory.bans.in_channel(client.name(), channel)) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "mode", r.mode);
        cJSON_AddStringToObject(item, "nick", r.nick);
        // seconds left; 0 = until lifted, -1 = timed, waiting for the clock
        cJSON_AddNumberToObject(item, "left", r.deadline > 0 ? std::max<int64_t>(r.deadline - now.steady, 1) : r.deadline);
        cJSON_AddBoolToObject(item, "mute", client.is_mute(r.mode));
        cJSON_AddItemToArray(bans, item);
    }
    return send_json(req, res, 200);
}

// {"action": "ban_lift", "network", "channel", "mode"}: moderators lift mutes, administrators bans too
esp_err_t WebServer::ban_lift(httpd_req_t* req, const cJSON* body, IRCClient& client, const std::string& channel,
                              const std::string& user, bool admin) {
    cJSON* mode_i = cJSON_GetObjectItemCaseSensitive(body, "mode");
    const std::string mode = cJSON_IsString(mode_i) ? mode_i->valuestring : "";
    if (!admin && !client.is_mute(mode)) return send_message(req, 403, "Administrator permission required to lift a ban.");
    const auto result = client.lift_ban(channel, mode);
    if (result.first) client.report(ReportKind::moderation, user + " (web panel): " + result.second);
    return send_message(req, result.first ? 200 : 400, result.second);
}
