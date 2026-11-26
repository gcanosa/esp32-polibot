// dynlimit, lockdown, unlock, entrymsg, badwords, join, part, say, rehash
// One function per subject: together they needed more stack than the calling tasks can spare.
#include "irc_client.h"
#include "irc_internal.h"
#include <algorithm>

bool IRCClient::dynlimit_command(Command& c) {
    const std::string& cmd = c.name;
    if (cmd != "dynlimit" && cmd != "limit") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    if (ch.empty()) {
        send_privmsg(c.reply_to, "Usage: dynlimit <#chan> <on|off|offset>");
        return true;
    }
    ChannelGuardianConfig cfg = guardian_.get_channel_config(name_, ch);
    if (c.args.empty()) {
        send_privmsg(c.reply_to, ch + " Dynamic Limit is " + (cfg.dynamic_limit.enabled ? "ON" : "OFF") +
                     " (offset: " + std::to_string(cfg.dynamic_limit.offset) + ")");
        return true;
    }
    const std::string sub = irc_lower(c.args[0]);
    const bool off = sub == "off" || sub == "disable";
    const bool on = sub == "on" || sub == "enable";
    const bool number_given = on ? c.args.size() > 1 : !off;
    if (number_given && !parse_number(on ? c.args[1] : sub, 0, 1000, cfg.dynamic_limit.offset)) {
        send_privmsg(c.reply_to, "Usage: dynlimit [#chan] <on [slots]|off|slots>  (slots: 0-1000)");
        return true;
    }
    cfg.dynamic_limit.enabled = !off;
    guardian_.save_channel_config(name_, ch, cfg);
    if (!check_and_update_dynamic_limit(ch) && off) send_raw("MODE " + ch + " -l");
    confirm(c, ch, off ? "Dynamic Limit DISABLED on " + ch
                   : on ? "Dynamic Limit ENABLED on " + ch
                        : "Dynamic Limit offset set to " + sub + " on " + ch);
    return true;
}

bool IRCClient::lockdown_command(Command& c) {
    const std::string& cmd = c.name;
    if (cmd != "lockdown" && cmd != "unlock") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    if (ch.empty()) {
        send_privmsg(c.reply_to, "Usage: " + cmd + " <#channel>");
        return true;
    }
    std::string modes = g_settings.get()->lockdown_modes;
    if (cmd == "lockdown") {
        lockdown(ch, false);
        confirm(c, ch, "EMERGENCY LOCKDOWN ACTIVATED on " + ch + " (" + modes + ").");
    } else {
        release_lockdown(ch);
        modes[0] = '-';
        confirm(c, ch, "Lockdown RELEASED on " + ch + " (" + modes + ").");
    }
    return true;
}

bool IRCClient::entrymsg_command(Command& c) {
    if (c.name != "entrymsg") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    if (ch.empty()) {
        send_privmsg(c.reply_to, "Usage: entrymsg <#chan> [<message>|off]");
        return true;
    }
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, ch);
    if (c.args.empty()) {
        const bool on = rules.entrymsg.enabled && !rules.entrymsg.message.empty();
        send_privmsg(c.reply_to, ch + " Entry Message: " + (on ? rules.entrymsg.message : "DISABLED"));
        return true;
    }
    const std::string first = irc_lower(c.args[0]);
    const bool off = c.args.size() == 1 && (first == "off" || first == "disable");
    rules.entrymsg.enabled = !off;
    if (!off) {
        rules.entrymsg.notice = true;
        rules.entrymsg.message = join_words(c.args);
    }
    guardian_.save_channel_config(name_, ch, rules);
    c.audit = true;
    send_privmsg(c.reply_to, "Entry message " + std::string(off ? "DISABLED" : "updated") + " for " + ch + ".");
    return true;
}

bool IRCClient::badwords_command(Command& c) {
    if (c.name != "badwords") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    const std::string sub = c.args.empty() ? "list" : irc_lower(c.args[0]);
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, ch);
    if (sub == "list") {
        send_privmsg(c.reply_to, "Global words: " + list_line(guardian_.get_global_badwords()));
        if (!ch.empty()) send_privmsg(c.reply_to, ch + " words: " + list_line(rules.badwords));
        return true;
    }
    if ((sub != "add" && sub != "del") || c.args.size() < 2 || ch.empty()) {
        send_privmsg(c.reply_to, "Usage: badwords [#chan] [list|add <word>|del <word>]");
        return true;
    }
    const std::string word = irc_lower(c.args[1]);
    auto it = std::find(rules.badwords.begin(), rules.badwords.end(), word);
    const bool present = it != rules.badwords.end();
    if ((sub == "add") == present) {
        send_privmsg(c.reply_to, "'" + word + (present ? "' is already on the list for " : "' is not on the list for ") + ch + ".");
        return true;
    }
    if (present) rules.badwords.erase(it);
    else rules.badwords.push_back(word);
    guardian_.save_channel_config(name_, ch, rules);
    c.audit = true;
    send_privmsg(c.reply_to, (present ? "Removed '" : "Added '") + word + (present ? "' from " : "' to ") + ch + ".");
    return true;
}

// Saves the channel into (or out of) this network's autojoin list, so it survives a reboot
bool IRCClient::set_autojoin(const std::string& channel, bool wanted, const std::string& key) {
    return config_mgr_.update([&](cJSON* root) {
        cJSON* irc = cJSON_GetObjectItemCaseSensitive(root, "irc");
        cJSON* net = nullptr;
        cJSON_ArrayForEach(net, cJSON_GetObjectItemCaseSensitive(irc, "networks")) {
            cJSON* network = cJSON_GetObjectItemCaseSensitive(net, "name");
            if (!cJSON_IsString(network) || name_ != network->valuestring) continue;
            cJSON* list = cJSON_GetObjectItemCaseSensitive(net, "channels");
            if (!cJSON_IsArray(list)) {
                cJSON_DeleteItemFromObjectCaseSensitive(net, "channels");
                list = cJSON_AddArrayToObject(net, "channels");
            }
            for (int i = cJSON_GetArraySize(list) - 1; i >= 0; i--) {
                cJSON* item = cJSON_GetArrayItem(list, i);
                if (cJSON_IsString(item) && same(item->valuestring, channel)) cJSON_DeleteItemFromArray(list, i);
            }
            if (wanted) cJSON_AddItemToArray(list, cJSON_CreateString(channel.c_str()));
            // its password goes with it: replaced when a new one is given, dropped with the channel
            cJSON* keys = cJSON_GetObjectItemCaseSensitive(net, "channel_keys");
            if (!wanted || !key.empty()) cJSON_DeleteItemFromObject(keys, channel.c_str()); // ignores case
            if (wanted && !key.empty()) {
                if (!keys) keys = cJSON_AddObjectToObject(net, "channel_keys");
                cJSON_AddStringToObject(keys, irc_lower(channel).c_str(), key.c_str());
            }
            return true;
        }
        return false;
    });
}

bool IRCClient::autojoin_command(Command& c) {
    const std::string& cmd = c.name;
    if (cmd != "join" && cmd != "part") return false;
    const bool joining = cmd == "join";
    std::string ch;
    if (!joining) resolve_channel(c, ch);
    else if (!c.args.empty()) ch = (c.args[0][0] == '#' || c.args[0][0] == '&') ? c.args[0] : "#" + c.args[0];
    if (!allowed(c, "admin")) return true;
    if (!is_channel_name(ch)) {
        send_privmsg(c.reply_to, "Usage: " + cmd + " <#channel>");
        return true;
    }
    if (is_report_channel(ch)) {
        send_privmsg(c.reply_to, ch + " is the report channel: change it in the web panel.");
        return true;
    }
    const std::string key = joining && c.args.size() > 1 ? c.args[1] : "";
    if (!key.empty() && !c.is_pm) {
        send_privmsg(c.reply_to, "Never type a channel password in a channel: send me this in a private message.");
        return true;
    }
    if (key.size() > MAX_JOIN_KEY || key.find(',') != std::string::npos || (!key.empty() && key[0] == ':')) {
        send_privmsg(c.reply_to, "That cannot be a channel password.");
        return true;
    }
    c.audit = true;
    const bool listed = contains_ci(settings()->channels, ch);
    if (!set_autojoin(ch, joining, key)) {
        send_privmsg(c.reply_to, "Could not save to flash.");
        return true;
    }
    g_irc_manager.apply_config(); // joins or parts it now
    if (!joining && !listed) send_raw("PART " + ch + " :Leaving per admin request");
    send_privmsg(c.reply_to, joining ? "Joining " + ch + " (saved to the autojoin list)."
                                     : "Leaving " + ch + " (removed from the autojoin list).");
    return true;
}

bool IRCClient::channel_command(Command& c) {
    if (dynlimit_command(c) || lockdown_command(c) || entrymsg_command(c) || badwords_command(c) || autojoin_command(c)) return true;
    const std::string& cmd = c.name;

    if (cmd == "say") {
        if (!allowed(c, "admin")) return true;
        if (c.args.size() < 2) {
            send_privmsg(c.reply_to, "Usage: say <#channel|nick> <message>");
            return true;
        }
        c.audit = true;
        send_privmsg(c.args[0], join_words(c.args, 1));
        if (!same(c.args[0], c.reply_to)) send_privmsg(c.reply_to, "Sent to " + c.args[0] + ".");
        return true;
    }

    if (cmd == "rehash") {
        if (!allowed(c, "admin")) return true;
        config_mgr_.rehash();
        c.audit = true;
        send_privmsg(c.reply_to, "Configuration successfully rehashed from flash!");
        return true;
    }
    return false;
}
