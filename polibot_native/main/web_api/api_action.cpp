// Buttons of the panel: device actions, channel actions and the IRC console
#include "web_server.h"
#include "alerts.h"
#include "mode_lock.h"
#include <algorithm>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 11. POST /api/action
esp_err_t WebServer::api_action_handler(httpd_req_t* req) {
    UserAccount user;
    bool has_auth = g_web_server.get_authenticated_user(req, user);

    std::string body = read_post_body(req);
    cJSON* p = cJSON_Parse(body.c_str());
    if (!p) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        return send_json(req, err, 400);
    }

    cJSON* act_i = cJSON_GetObjectItemCaseSensitive(p, "action");
    std::string action = (act_i && act_i->valuestring) ? act_i->valuestring : "";

    int user_level = has_auth ? get_role_level(user.global_role) : 0;

    if (action == "test_alert") {
        cJSON_Delete(p);
        if (user_level < get_role_level("admin")) return send_message(req, 403, "Administrator permission required.");
        const std::string& url = g_settings.get()->alert_url;
        if (url.empty()) return send_message(req, 400, "Save an alert address first.");
        alert("Test alert, asked for by " + user.username + ".");
        return send_message(req, 200, "Test alert queued: the event log says whether it arrived.");
    }

    if (action == "rehash" || action == "reconnect" || action == "reboot" || action == "clear_logs") {
        if (user_level < get_role_level("admin")) {
            cJSON_Delete(p);
            cJSON* err = cJSON_CreateObject();
            cJSON_AddStringToObject(err, "status", "forbidden");
            return send_json(req, err, 403);
        }
        if (action == "rehash") {
            g_config_mgr.rehash();
        } else if (action == "reconnect") {
            cJSON* net_i = cJSON_GetObjectItemCaseSensitive(p, "network");
            auto client = (net_i && net_i->valuestring && net_i->valuestring[0]) ? g_irc_manager.find(net_i->valuestring) : nullptr;
            if (client) client->reconnect();
            else g_irc_manager.reconnect_all();
        } else if (action == "clear_logs") {
            g_log_buffer.clear();
            g_log_buffer.log("Event log cleared by " + std::string(user.username) + ".");
        } else if (action == "reboot") {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        cJSON_Delete(p);
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "status", "ok");
        return send_json(req, res, 200);
    }

    const bool lists = action == "access_add" || action == "access_del";
    if (lists || action == "chan_lockdown" || action == "chan_unlock" || action == "chan_limit_update" || action == "save_chan_config" ||
        action == "channel_view" || action == "ban_lift") {
        cJSON* ch_i = cJSON_GetObjectItemCaseSensitive(p, "channel");
        std::string ch = (ch_i && ch_i->valuestring) ? ch_i->valuestring : "";

        cJSON* net_i = cJSON_GetObjectItemCaseSensitive(p, "network");
        auto client = g_irc_manager.find((net_i && net_i->valuestring) ? net_i->valuestring : "");
        if (!client || ch.empty()) {
            cJSON_Delete(p);
            return send_message(req, 400, "Unknown network or channel.");
        }
        const std::string net = client->name();

        if (!has_auth || !g_security_mgr.has_permission(net, user.username, WEB_CONSOLE_HOSTMASK, "mod", ch)) {
            cJSON_Delete(p);
            return send_message(req, 403, "Moderator permission required.");
        }
        // who gets modes by themselves is for administrators to say
        const bool admin = g_security_mgr.has_permission(net, user.username, WEB_CONSOLE_HOSTMASK, "admin", ch);
        if (lists) {
            if (!admin) {
                cJSON_Delete(p);
                return send_message(req, 403, "Administrator permission required.");
            }
            const esp_err_t result = access_action(req, p, *client, ch, user.username, action == "access_add");
            cJSON_Delete(p);
            return result;
        }

        if (action == "channel_view" || action == "ban_lift") {
            const esp_err_t result = action == "ban_lift" ? ban_lift(req, p, *client, ch, user.username, admin) : channel_view(req, *client, ch);
            cJSON_Delete(p);
            return result;
        }

        if (action == "chan_lockdown") {
            client->lockdown(ch, false);
            client->report(ReportKind::raid, user.username + " locked down " + ch + " from the web panel.");
        } else if (action == "chan_unlock") {
            client->release_lockdown(ch);
            client->report(ReportKind::raid, user.username + " unlocked " + ch + " from the web panel.");
        } else if (action == "chan_limit_update") {
            client->check_and_update_dynamic_limit(ch);
        } else if (action == "save_chan_config") {
            cJSON* c_obj = cJSON_GetObjectItemCaseSensitive(p, "config");
            if (c_obj) {
                // Start from the stored rules: the panel does not send badwords or lockdown
                ChannelGuardianConfig cfg = g_guardian.get_channel_config(net, ch);
                cJSON* dyn = cJSON_GetObjectItemCaseSensitive(c_obj, "dynamic_limit");
                if (dyn) {
                    cJSON* en = cJSON_GetObjectItemCaseSensitive(dyn, "enabled");
                    cJSON* off = cJSON_GetObjectItemCaseSensitive(dyn, "offset");
                    cJSON* ml = cJSON_GetObjectItemCaseSensitive(dyn, "min_limit");
                    if (en) cfg.dynamic_limit.enabled = cJSON_IsTrue(en);
                    if (off) cfg.dynamic_limit.offset = off->valueint;
                    if (ml) cfg.dynamic_limit.min_limit = ml->valueint;
                }
                cJSON* em = cJSON_GetObjectItemCaseSensitive(c_obj, "entrymsg");
                if (em) {
                    cJSON* en = cJSON_GetObjectItemCaseSensitive(em, "enabled");
                    cJSON* nt = cJSON_GetObjectItemCaseSensitive(em, "notice");
                    cJSON* msg = cJSON_GetObjectItemCaseSensitive(em, "message");
                    if (en) cfg.entrymsg.enabled = cJSON_IsTrue(en);
                    if (nt) cfg.entrymsg.notice = cJSON_IsTrue(nt);
                    if (msg && msg->valuestring) cfg.entrymsg.message = msg->valuestring;
                }
                cJSON* lk = cJSON_GetObjectItemCaseSensitive(c_obj, "lockdown");
                if (lk) cfg.lockdown = cJSON_IsTrue(lk);
                cJSON* words = cJSON_GetObjectItemCaseSensitive(c_obj, "badwords");
                if (cJSON_IsArray(words)) {
                    cfg.badwords.clear();
                    cJSON* w = nullptr;
                    cJSON_ArrayForEach(w, words) {
                        if (cJSON_IsString(w) && w->valuestring[0]) cfg.badwords.push_back(irc_lower(w->valuestring));
                    }
                }

                cJSON* voice = cJSON_GetObjectItemCaseSensitive(c_obj, "timed_voice");
                if (admin && voice) {
                    cJSON* en = cJSON_GetObjectItemCaseSensitive(voice, "enabled");
                    cJSON* minutes = cJSON_GetObjectItemCaseSensitive(voice, "minutes");
                    if (en) cfg.timed_voice.enabled = cJSON_IsTrue(en);
                    if (cJSON_IsNumber(minutes)) cfg.timed_voice.minutes = std::min(std::max(minutes->valueint, 1), MAX_VOICE_MINUTES);
                }
                cJSON* op = cJSON_GetObjectItemCaseSensitive(c_obj, "auto_op"); // null: as the general setting says
                if (admin && op) cfg.auto_op = cJSON_IsBool(op) ? cJSON_IsTrue(op) : -1;

                cJSON* topic = cJSON_GetObjectItemCaseSensitive(c_obj, "topic_lock");
                if (admin && topic) {
                    cJSON* en = cJSON_GetObjectItemCaseSensitive(topic, "enabled");
                    cJSON* text = cJSON_GetObjectItemCaseSensitive(topic, "topic");
                    if (en) cfg.topic_lock = cJSON_IsTrue(en);
                    if (cJSON_IsString(text)) cfg.topic = text->valuestring;
                    cfg.topic.erase(std::remove_if(cfg.topic.begin(), cfg.topic.end(), [](char c) { return c == '\r' || c == '\n'; }), cfg.topic.end());
                }
                cJSON* modes = cJSON_GetObjectItemCaseSensitive(c_obj, "mode_lock");
                if (admin && cJSON_IsString(modes)) {
                    cfg.mode_lock = clean_mode_lock(modes->valuestring);
                    if (cfg.mode_lock.empty() && modes->valuestring[0]) {
                        cJSON_Delete(p);
                        return send_message(req, 400, "Only plain channel modes can be locked, such as +nt-s (no ranks, lists, keys or limits).");
                    }
                }

                g_guardian.save_channel_config(net, ch, cfg);
                client->check_and_update_dynamic_limit(ch);
                client->review_modes(ch);
                client->enforce_locks(ch);
            }
        }
        cJSON_Delete(p);
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "status", "ok");
        return send_json(req, res, 200);
    }

    if (action == "send_irc") {
        cJSON* cmd_i = cJSON_GetObjectItemCaseSensitive(p, "cmd");
        std::string cmd = (cmd_i && cmd_i->valuestring) ? cmd_i->valuestring : "";
        if (!has_auth) {
            cJSON_Delete(p);
            return send_message(req, 403, "Login required.");
        }
        std::string uname = user.username;
        cJSON* net_i = cJSON_GetObjectItemCaseSensitive(p, "network");
        auto client = g_irc_manager.find((net_i && net_i->valuestring) ? net_i->valuestring : "");
        if (!client) {
            cJSON_Delete(p);
            return send_message(req, 400, "No such IRC network.");
        }

        std::string replies; // what the bot answers, which goes to the event log
        if (cmd.rfind("!", 0) == 0) {
            const uint32_t before = g_log_buffer.latest();
            client->handle_command("", uname, WEB_CONSOLE_HOSTMASK, cmd.substr(1), true);
            const std::string mark = "[" + client->name() + "] Reply: ";
            for (const auto& line : g_log_buffer.get_lines(before)) {
                if (line.message.rfind(mark, 0) == 0) replies += (replies.empty() ? "" : "\n") + line.message.substr(mark.size());
            }
        } else {
            if (user_level < get_role_level("admin")) {
                cJSON_Delete(p);
                cJSON* err = cJSON_CreateObject();
                cJSON_AddStringToObject(err, "status", "forbidden");
                return send_json(req, err, 403);
            }
            client->send_raw(cmd);
        }
        g_log_buffer.log("Web Console (" + uname + " on " + client->name() + "): " + redact_command(cmd));
        cJSON_Delete(p);
        return send_message(req, 200, replies);
    }

    cJSON_Delete(p);
    cJSON* err = cJSON_CreateObject();
    cJSON_AddStringToObject(err, "status", "unknown_action");
    return send_json(req, err, 400);
}
