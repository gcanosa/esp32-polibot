// IRCClient: acting on rule breakers: kicks, bans, mutes, lockdowns
#include "irc_client.h"
#include "irc_internal.h"
#include "bot_memory.h"
#include <algorithm>

static constexpr size_t AKICK_SWEEP = 20;        // removals per look at a channel

// One look at a channel's +l: on an interval from tick(), and at once when the bot is opped or the
// settings change
bool IRCClient::check_and_update_dynamic_limit(const std::string& channel) {
    if (is_report_channel(channel)) return false;
    const DynamicLimitConfig wanted = guardian_.get_channel_config(name_, channel).dynamic_limit;
    const auto settings = g_settings.get();
    std::string change, why;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end() || it->second.op == 0 || it->second.limit_from < 0) return false;
        ChannelState& state = it->second;
        const int64_t now = seconds_now();
        if (!wanted.enabled) {
            if (state.limit.current) change = "-l"; // switched off: take back what the bot set
            why = "switched off";
            state.limit = FloatingLimit();
        } else if (now < state.limit_from) {
            // just joined: after a restart or a netsplit everyone comes back at once
            if (state.lift_limit) change = "-l";
            why = "just joined, no limit for " + format_duration(state.limit_from - now);
            state.lift_limit = false;
        } else {
            const int users = (int)state.members.size() + (now < state.split_until ? state.split_away : 0);
            const LimitRules rules{wanted.offset, wanted.min_limit, settings->limit_slack, settings->limit_lower_after};
            const int next = state.limit.next(users, rules, now);
            if (next) change = "+l " + std::to_string(next);
            why = "users: " + std::to_string(users);
        }
    }
    if (change.empty()) return false;
    send_raw("MODE " + channel + " " + change);
    log("Channel limit: " + channel + " " + change + " (" + why + ")");
    return true;
}

bool IRCClient::moderate(const std::string& channel, const std::string& nick, const std::string& hostmask, const std::string& message) {
    if (is_report_channel(channel)) return false;
    if (g_settings.get()->exempt_ops) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it != channels_.end()) {
            const Member* member = it->second.members.find(nick);
            if (member && member->is_operator()) return false;
        }
    }
    bool stopped = false;
    for (const auto& act : guardian_.inspect_message(name_, channel, nick, hostmask, message)) {
        execute_action(act);
        stopped = stopped || act.type == "kick" || act.type == "ban" || act.type == "quiet" || act.type == "lockdown";
    }
    return stopped;
}

std::string IRCClient::member_userhost(const std::string& nick) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& entry : channels_) {
            const Member* member = entry.second.members.find(nick);
            if (member && member->userhost[0]) return member->userhost;
        }
    }
    SeenRecord seen;
    return g_memory.seen.find(name_, nick, seen) ? seen.userhost : "";
}

std::string IRCClient::mask_for(const std::string& target) {
    const std::string mask = ban_mask(target, member_userhost(target), g_settings.get()->mask_style);
    if (target.find_first_of("!@$~") != std::string::npos) return mask;
    // a host shared with the bot (a bouncer, one LAN) must not get the bot banned too
    const std::string me = current_nick();
    const std::string mine = member_userhost(me);
    if (!mine.empty() && wildcard_match(mask, me + "!" + mine)) return target + "!*@*";
    return mask;
}

void IRCClient::execute_action(const GuardianAction& act) {
    if (act.type == "notice") {
        send_notice(act.target, act.message);
    } else if (act.type == "privmsg") {
        send_privmsg(act.target, act.message);
    } else if (act.type == "report") {
        report(ReportKind::raid, act.reason + " (" + (act.nick.empty() ? "" : act.nick + " in ") + act.channel + ")");
    } else if (act.type == "warn") {
        send_privmsg(act.channel, act.nick + ": WARNING - " + act.reason);
        report(ReportKind::moderation, "Warned " + act.nick + " in " + act.channel + ": " + act.reason);
    } else if (!can_act(act.channel)) { // everything below needs operator status
        cannot_act(act.channel, act.type == "mode" ? "set " + act.mode : act.type == "lockdown" ? "lock down" : act.type + " " + act.nick);
    } else if (act.type == "mode") {
        send_raw("MODE " + act.channel + " " + act.mode);
    } else if (act.type == "lockdown") {
        raid_lockdown(act);
    } else if (act.type == "kick" || act.type == "ban" || act.type == "quiet" || act.type == "akick") {
        remove_or_mute(act);
    }
}

void IRCClient::raid_lockdown(const GuardianAction& act) {
    if (guardian_.get_channel_config(name_, act.channel).lockdown) return;
    lockdown(act.channel, true);
    const int after = g_settings.get()->unlock_after_seconds;
    report(ReportKind::raid, "Locked down " + act.channel + (after ? " for " + format_duration(after) : " until unlocked") + ". " + act.reason);
}

void IRCClient::remove_or_mute(const GuardianAction& act) {
    const std::string kick = "KICK " + act.channel + " " + act.nick + " :" + act.reason;
    std::string type = act.type;
    const auto cfg = settings();
    if (type == "quiet") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        // on some servers +q means "channel owner": removing the user is the safe choice there
        if (cfg->quiet_mode.size() < 2 || support_.prefix_modes.find(cfg->quiet_mode[1]) != std::string::npos) type = "kick";
    }
    if (type == "kick") {
        send_raw(kick);
        report(ReportKind::moderation, "Kicked " + act.nick + " from " + act.channel + ": " + act.reason);
        return;
    }
    // a ban (by rule or by the auto-kick list) or a mute: set it, remember it, say so
    const bool listed = type == "akick";
    const std::string change = type == "quiet" ? fill_template(cfg->quiet_mode, act.channel, mask_for(act.nick))
                                               : "+b " + (listed ? act.mode : mask_for(act.nick));
    if (type == "quiet") devoice_restricted(act.channel, change); // a ban is followed by a kick
    send_raw("MODE " + act.channel + " " + change);
    g_memory.bans.add(name_, act.channel, change, listed ? "" : act.nick, listed ? 0 : act.seconds, bot_clock());
    const std::string lasting = !listed && act.seconds ? " for " + format_duration(act.seconds) : "";
    if (type == "quiet") send_notice(act.nick, "You have been muted in " + act.channel + lasting + ": " + act.reason);
    else send_raw(kick);
    const char* done = listed ? "Auto-kicked " : type == "ban" ? "Banned " : "Muted ";
    report(ReportKind::moderation, done + act.nick + " (" + change + ") in " + act.channel + lasting + ": " + act.reason);
}

void IRCClient::lockdown(const std::string& channel, bool automatic) {
    const auto settings = g_settings.get();
    send_raw("MODE " + channel + " " + settings->lockdown_modes); // first: saving takes a while
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    rules.lockdown = true;
    rules.auto_lockdown = automatic;
    guardian_.save_channel_config(name_, channel, rules);
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(irc_lower(channel));
    if (it != channels_.end()) {
        it->second.unlock_at = automatic && settings->unlock_after_seconds ? seconds_now() + settings->unlock_after_seconds : 0;
    }
}

void IRCClient::release_lockdown(const std::string& channel) {
    std::string modes = g_settings.get()->lockdown_modes;
    modes[0] = '-';
    send_raw("MODE " + channel + " " + modes);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it != channels_.end()) it->second.unlock_at = 0;
    }
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    rules.lockdown = false;
    rules.auto_lockdown = false;
    guardian_.save_channel_config(name_, channel, rules);
}

void IRCClient::sweep_akicks(const std::string& channel) {
    if (is_report_channel(channel) || !can_act(channel) || guardian_.get_akicks().empty()) return;
    const std::string me = current_nick();
    std::vector<std::pair<std::string, std::string>> present; // nickname, full hostmask
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return;
        for (const auto& m : it->second.members.all()) {
            if (m.userhost[0] && !m.is_operator() && !same(m.nick, me)) present.push_back({m.nick, std::string(m.nick) + "!" + m.userhost});
        }
    }
    size_t removed = 0;
    for (const auto& user : present) {
        AkickEntry listed;
        if (!guardian_.akick_match(name_, channel, user.second, listed)) continue;
        if (security_mgr_.has_permission(name_, user.first, user.second, "mod", channel)) continue;
        GuardianAction act;
        act.type = "akick";
        act.channel = channel;
        act.nick = user.first;
        act.mode = listed.mask;
        act.reason = listed.reason.empty() ? "You are on the auto-kick list" : listed.reason;
        execute_action(act);
        if (++removed >= AKICK_SWEEP) break;
    }
}

void IRCClient::lift_expired_bans() {
    for (const auto& ban : g_memory.bans.expired(name_, bot_clock())) {
        bool able;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(ban.channel));
            able = it != channels_.end() && it->second.op != 0;
        }
        if (!able) continue; // kept until the bot can lift it
        std::string change = ban.mode;
        if (change.empty()) continue;
        change[0] = '-';
        if (!send_raw(std::string("MODE ") + ban.channel + " " + change)) continue;
        g_memory.bans.remove(name_, ban.channel, ban.mode);
        report(ReportKind::moderation, std::string("Time is up: ") + change + " in " + ban.channel + ".");
    }
}

void IRCClient::finish_masskick(const std::string& channel) {
    MassKick job;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!masskick_.active || !same(masskick_.channel, channel)) return;
        job = masskick_;
        masskick_ = MassKick();
    }
    size_t kicked = 0;
    for (const auto& nick : job.victims) { // the send queue paces them
        if (!send_raw("KICK " + job.channel + " " + nick + " :" + job.reason)) break;
        kicked++;
    }
    std::string result = "Masskick complete: " + std::to_string(kicked) + " user(s) matching '" + job.pattern + "' removed from " + job.channel + ".";
    if (job.skipped) result += " " + std::to_string(job.skipped) + " more matched: run it again.";
    send_privmsg(job.reply_target, result);
}
