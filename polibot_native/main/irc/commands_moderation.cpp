// op, voice, half, owner and their "de" forms, kick, invite, topic, quiet, unquiet, ban, kickban, unban, bans
#include "irc_client.h"
#include "irc_internal.h"
#include "bot_memory.h"
#include <algorithm>

// quiet, unquiet, ban, kickban, unban
bool IRCClient::ban_command(Command& c) {
    const std::string& cmd = c.name;
    std::vector<std::string>& args = c.args;
    const bool kicking = cmd == "kickban" || cmd == "kb";
    if (cmd != "quiet" && cmd != "unquiet" && cmd != "ban" && cmd != "unban" && !kicking) return false;

    std::string ch;
    resolve_channel(c, ch);
    const bool quiet = cmd == "quiet" || cmd == "unquiet";
    const bool adding = cmd.rfind("un", 0) != 0;
    if (!allowed(c, quiet ? "mod" : "admin", ch)) return true;
    const std::string usage = "Usage: " + cmd + " [#chan] " +
                              (kicking ? "<nick> [time] [reason]" : std::string("<nick|mask>") + (adding ? " [time, e.g. 30m, 2h, 7d]" : ""));
    if (args.empty() || ch.empty() || (kicking && args[0].find_first_of("!@$~*?") != std::string::npos)) {
        send_privmsg(c.reply_to, usage);
        return true;
    }
    if (kicking && same(args[0], current_nick())) {
        send_privmsg(c.reply_to, "I will not kick myself.");
        return true;
    }
    const auto limits = g_settings.get();
    int seconds = (quiet ? limits->quiet_minutes : limits->ban_minutes) * 60;
    const bool timed = adding && args.size() > 1 && parse_duration(args[1], seconds);
    // after "ban" and "quiet" only a time may follow; after "kickban" it may be left out before the reason
    if (adding && !kicking && args.size() > 1 && !timed) {
        send_privmsg(c.reply_to, usage);
        return true;
    }
    const auto cfg = settings();
    if (quiet) {
        bool means_owner;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            means_owner = cfg->quiet_mode.size() > 1 && support_.prefix_modes.find(cfg->quiet_mode[1]) != std::string::npos;
        }
        if (cfg->quiet_mode.empty() || means_owner) {
            send_privmsg(c.reply_to, cfg->quiet_mode.empty() ? "Muting is not set up for this network (web panel, IRC Settings)."
                                     : "On this server that mode is not a mute: set another mute mode in the web panel (IRC Settings).");
            return true;
        }
    }
    auto change_for = [&](const std::string& mask) { return quiet ? fill_template(cfg->quiet_mode, ch, mask) : "+b " + mask; };

    if (adding) {
        const std::string change = change_for(mask_for(args[0]));
        devoice_restricted(ch, change);
        send_raw("MODE " + ch + " " + change);
        const bool by_nick = args[0].find_first_of("!@$~*?") == std::string::npos;
        g_memory.bans.add(name_, ch, change, by_nick ? args[0] : "", seconds, bot_clock());
        if (kicking) { // after the ban, so that they cannot come straight back
            const size_t reason = timed ? 2 : 1;
            send_raw("KICK " + ch + " " + args[0] + " :" + (args.size() > reason ? join_words(args, reason) : "Banned"));
        }
        confirm(c, ch, "Executed MODE " + ch + " " + change + (seconds ? " (for " + format_duration(seconds) + ")" : "") +
                       (kicking ? " and kicked " + args[0] : ""));
        return true;
    }
    // lift what the bot set against this nickname; otherwise what the argument stands for
    std::vector<std::string> changes;
    for (const auto& mode : g_memory.bans.for_nick(name_, ch, args[0])) {
        if (is_mute(mode) == quiet) changes.push_back(mode);
    }
    if (changes.empty()) changes.push_back(change_for(mask_for(args[0])));
    std::string done;
    for (auto change : changes) {
        g_memory.bans.remove(name_, ch, change);
        change[0] = '-';
        send_raw("MODE " + ch + " " + change);
        done += (done.empty() ? "" : ", ") + change;
    }
    confirm(c, ch, "Executed MODE " + ch + " " + done);
    return true;
}

// bans: what the bot has set in a channel, told to the asker alone
bool IRCClient::bans_command(Command& c) {
    if (c.name != "bans") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "mod", ch)) return true;
    // masks name people's hosts: never listed in the channel
    const std::string to = c.is_pm || c.reply_to.empty() ? c.reply_to : c.nick;
    if (ch.empty()) {
        send_privmsg(to, "Usage: bans [#chan]");
        return true;
    }
    const auto records = g_memory.bans.in_channel(name_, ch);
    const Clock now = bot_clock();
    std::vector<std::string> entries;
    for (const auto& r : records) {
        const std::string left = r.deadline > 0 ? format_duration(r.deadline - now.steady) + " left"
                               : r.deadline < 0 ? "timed" : "until lifted";
        entries.push_back(std::string(r.mode) + (r.nick[0] ? std::string(" (") + r.nick + ", " : " (") + left + ")");
    }
    if (entries.empty()) {
        send_privmsg(to, "I have no bans or mutes on record for " + ch + ".");
        return true;
    }
    // the newest first, a few lines at most
    std::reverse(entries.begin(), entries.end());
    size_t next = 0;
    for (int line = 0; line < 5 && next < entries.size(); line++) {
        std::string text = (line ? "" : ch + ": ");
        while (next < entries.size() && (text.size() + entries[next].size() < 380 || text.size() < 80)) text += entries[next++] + "  ";
        send_privmsg(to, text);
    }
    if (next < entries.size()) send_privmsg(to, std::to_string(entries.size() - next) + " more are listed in the web panel.");
    return true;
}

// op, voice, half, owner and their "de" forms: for the nicks named, or else for whoever asks
bool IRCClient::status_command(Command& c) {
    const std::string& cmd = c.name;
    const char mode = cmd == "op" || cmd == "deop" ? 'o'
                    : cmd == "voice" || cmd == "devoice" ? 'v'
                    : cmd == "half" || cmd == "halfop" || cmd == "hop" || cmd == "dehalf" || cmd == "dehalfop" || cmd == "dehop" ? 'h'
                    : cmd == "owner" || cmd == "deowner" ? 'q' : 0;
    if (!mode) return false;
    const bool adding = cmd.rfind("de", 0) != 0;

    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, mode == 'q' ? "owner" : "mod", ch)) return true;
    std::vector<std::string> nicks = c.args;
    if (nicks.empty() && c.hostmask != WEB_CONSOLE_HOSTMASK) nicks.push_back(c.nick);
    if (nicks.empty() || ch.empty()) {
        send_privmsg(c.reply_to, "Usage: " + cmd + " [#chan] [nick...]");
        return true;
    }

    bool offered, entitled = true;
    size_t per_line;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        offered = support_.prefix_modes.find(mode) != std::string::npos;
        per_line = support_.max_modes;
        if (mode == 'q') { // only an owner may make or unmake one
            auto it = channels_.find(irc_lower(ch));
            const Member* me = it == channels_.end() ? nullptr : it->second.members.find(current_nick_.empty() ? cfg_->nick : current_nick_);
            entitled = me && me->has_mode('q');
        }
    }
    // never send a letter the server does not use for a rank: elsewhere +q is a mute
    if (!offered) {
        send_privmsg(c.reply_to, std::string("This server has no ") + (mode == 'q' ? "channel owner" : "half-operator") + " status.");
        return true;
    }
    if (!entitled) {
        send_privmsg(c.reply_to, "I am not an owner of " + ch + " myself, so I cannot do that.");
        return true;
    }

    for (size_t first = 0; first < nicks.size(); first += per_line) {
        std::string letters(adding ? "+" : "-"), names;
        for (size_t i = first; i < std::min(nicks.size(), first + per_line); i++) {
            letters += mode;
            names += " " + nicks[i];
        }
        send_raw("MODE " + ch + " " + letters + names);
    }
    confirm(c, ch, "Executed MODE " + ch + " " + (adding ? "+" : "-") + mode + " " + join_words(nicks));
    return true;
}

// kick, invite, topic
bool IRCClient::moderator_command(Command& c) {
    const std::string& cmd = c.name;
    std::vector<std::string>& args = c.args;
    const std::string& sender_nick = c.nick;
    const std::string& hostmask = c.hostmask;
    const std::string& reply_target = c.reply_to;

    if (cmd == "kick") {
        std::string ch;
        resolve_channel(c, ch);
        if (!security_mgr_.has_permission(name_, sender_nick, hostmask, "mod", ch)) {
            send_privmsg(reply_target, "Permission denied.");
            return true;
        }
        if (args.empty() || ch.empty()) {
            send_privmsg(reply_target, "Usage: kick [#chan] <nick> [reason]");
            return true;
        }
        std::string target_user = args[0];
        std::string reason = "Kicked by Moderator";
        if (args.size() > 1) {
            reason = "";
            for (size_t i = 1; i < args.size(); i++) {
                if (i > 1) reason += " ";
                reason += args[i];
            }
        }
        send_raw("KICK " + ch + " " + target_user + " :" + reason);
        confirm(c, ch, "Kicked " + target_user + " from " + ch);
        return true;
    }

    if (cmd == "invite") {
        std::string ch;
        resolve_channel(c, ch);
        if (!allowed(c, "mod", ch)) return true;
        if (args.size() != 1 || ch.empty()) {
            send_privmsg(reply_target, "Usage: invite [#chan] <nick>");
            return true;
        }
        send_raw("INVITE " + args[0] + " " + ch);
        confirm(c, ch, "Invited " + args[0] + " to " + ch);
        return true;
    }

    if (cmd == "topic") {
        std::string ch;
        resolve_channel(c, ch);
        if (!security_mgr_.has_permission(name_, sender_nick, hostmask, "mod", ch)) {
            send_privmsg(reply_target, "Permission denied.");
            return true;
        }
        if (ch.empty()) {
            send_privmsg(reply_target, "Usage: topic [#chan] <new topic>");
            return true;
        }
        const std::string top = join_words(args);
        ChannelGuardianConfig rules = guardian_.get_channel_config(name_, ch);
        if (rules.topic_lock) {
            if (!security_mgr_.has_permission(name_, sender_nick, hostmask, "admin", ch)) {
                send_privmsg(reply_target, "The topic of " + ch + " is locked: an administrator can change it.");
                return true;
            }
            rules.topic = top;
            guardian_.save_channel_config(name_, ch, rules);
        }
        send_raw("TOPIC " + ch + " :" + top);
        confirm(c, ch, "Topic updated for " + ch);
        return true;
    }

    return false;
}
