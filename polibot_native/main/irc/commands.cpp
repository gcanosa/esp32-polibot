// IRC commands: who may use them, and which group handles which
#include "irc_client.h"
#include "irc_internal.h"
#include <algorithm>
#include <sstream>

// The role the help list is written for: the one that counts here, or in private the highest held anywhere
int IRCClient::help_level(const Command& c) {
    const bool in_channel = !c.target.empty() && c.target[0] == '#';
    int level = get_role_level(security_mgr_.get_role(name_, c.nick, c.hostmask, in_channel ? c.target : ""));
    UserAccount account;
    if (!in_channel && security_mgr_.get_effective_user(name_, c.nick, c.hostmask, account)) {
        for (const auto& r : account.channel_roles) {
            const size_t slash = r.first.find('/');
            if (slash == std::string::npos || r.first.substr(0, slash) == irc_lower(name_)) level = std::max(level, get_role_level(r.second));
        }
    }
    return level;
}

// People without a role get a limited number of commands, each and all together
bool IRCClient::admit_command(const Command& c) {
    if (help_level(c) >= get_role_level("mod")) return true;
    const auto limits = g_settings.get();
    const std::string who = user_host(c.hostmask).empty() ? c.nick : user_host(c.hostmask);
    const int64_t now = seconds_now();
    int own, everyone = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        own = commands_.hit(irc_lower(who), now, limits->command_seconds);
        if (own <= limits->command_count) everyone = commands_.hit("", now, limits->command_seconds);
    }
    if (own == limits->command_count + 1) report(ReportKind::security, "Ignoring commands from " + c.hostmask + " for a moment (too many).");
    return own <= limits->command_count && everyone <= limits->command_total;
}

// The reply to a command that needs operator status: honest when the bot does not have it
void IRCClient::confirm(Command& c, const std::string& channel, const std::string& done) {
    c.audit = true;
    if (can_act(channel)) {
        send_privmsg(c.reply_to, done);
        return;
    }
    request_ops(channel);
    send_privmsg(c.reply_to, "I am not an operator in " + channel + ", so the server will probably refuse that.");
}

bool IRCClient::allowed(const Command& c, const char* role, const std::string& channel) {
    if (security_mgr_.has_permission(name_, c.nick, c.hostmask, role, channel)) return true;
    send_privmsg(c.reply_to, "Permission denied.");
    return false;
}

// The channel a command is about: given as first argument, else the one it was sent to, else the only one joined
void IRCClient::resolve_channel(Command& c, std::string& channel) {
    if (!c.args.empty() && (c.args[0][0] == '#' || c.args[0][0] == '&')) {
        channel = c.args[0];
        c.args.erase(c.args.begin());
        return;
    }
    if (!c.is_pm && !c.target.empty() && c.target[0] == '#') {
        channel = c.target;
        return;
    }
    const std::vector<std::string> joined = get_joined_channels();
    channel = joined.size() == 1 ? joined.front() : "";
}

void IRCClient::handle_command(const std::string& target, const std::string& sender_nick, const std::string& hostmask, const std::string& cmd_string, bool is_pm) {
    std::istringstream iss(cmd_string);
    Command c;
    iss >> c.name;
    if (c.name.empty()) return;
    c.name = irc_lower(c.name);
    std::string token;
    while (iss >> token) c.args.push_back(token);
    c.target = target;
    c.nick = sender_nick;
    c.hostmask = hostmask;
    c.is_pm = is_pm;
    // An empty target sends replies to the panel log instead of whoever holds that nick on IRC
    c.reply_to = (hostmask == WEB_CONSOLE_HOSTMASK) ? "" : (is_pm ? sender_nick : target);

    if (hostmask != WEB_CONSOLE_HOSTMASK && !admit_command(c)) return;

    // what the report channel is told, if the command changes something; passwords and message texts stay out
    const size_t words = c.name == "passwd" || c.name == "password" ? 0
                       : c.name == "admin" ? 2
                       : c.name == "say" || c.name == "raw" || c.name == "join" || c.name == "topiclock" ? 1 : c.args.size();
    std::string asked = c.name;
    for (size_t i = 0; i < std::min(words, c.args.size()); i++) asked += " " + c.args[i];
    if (!is_pm) asked += " (in " + target + ")";

    // each handler returns true once it has dealt with the command
    if (!session_command(c) && !password_command(c) && !info_command(c) && !seen_command(c) && !tell_command(c) && !akick_command(c) &&
        !status_command(c) && !moderator_command(c) && !ban_command(c) && !bans_command(c) && !access_command(c) && !timevoice_command(c) &&
        !lock_command(c) && !channel_command(c)) owner_command(c);
    if (c.audit) report(ReportKind::moderation, c.nick + " used: " + asked);
}
