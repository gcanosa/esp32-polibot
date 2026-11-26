// login, logout, whoami, help, version, status, time, chanstats
#include "irc_client.h"
#include "irc_internal.h"
#include "command_help.h"
#include "system_info.h"
#include "ntp_manager.h"
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_app_desc.h>
#include <strings.h>

// login, logout, whoami
bool IRCClient::session_command(Command& c) {
    const std::string& cmd = c.name;
    std::vector<std::string>& args = c.args;
    const std::string& target = c.target;
    const std::string& sender_nick = c.nick;
    const std::string& hostmask = c.hostmask;
    const std::string& reply_target = c.reply_to;
    const bool is_pm = c.is_pm;

    // 0. AUTH & SESSIONS
    if (cmd == "login" || cmd == "auth") {
        if (!is_pm) {
            send_privmsg(target, sender_nick + ": For security, please login via Private Message (/msg " + current_nick() + " login <password>).");
            return true;
        }
        if (args.empty()) {
            send_privmsg(reply_target, "Usage in PM: login <password>  OR  login <username> <password>");
            return true;
        }
        auto res = args.size() == 1 ? security_mgr_.irc_login(name_, sender_nick, hostmask, args[0])
                                    : security_mgr_.irc_login(name_, sender_nick, hostmask, args[1], args[0]);
        send_privmsg(reply_target, res.second);
        report(ReportKind::security, hostmask + ": " + res.second);
        if (res.first) review_modes(); // what identification entitles them to
        return true;
    }

    if (cmd == "logout") {
        auto res = security_mgr_.irc_logout(name_, sender_nick);
        send_privmsg(reply_target, res.second);
        return true;
    }

    if (cmd == "whoami") {
        UserAccount u;
        if (security_mgr_.get_effective_user(name_, sender_nick, hostmask, u)) {
            send_privmsg(reply_target, "Identified as '" + u.username + "' [Global: " + u.global_role + "]");
        } else {
            send_privmsg(reply_target, "Not logged in (Role: USER). Send 'login <password>' in PM to authenticate.");
        }
        return true;
    }
    return false;
}

// help, version, status, time, chanstats
bool IRCClient::info_command(Command& c) {
    const std::string& cmd = c.name;
    const std::string& sender_nick = c.nick;
    const std::string& reply_target = c.reply_to;

    // 1. HELP
    if (cmd == "help") {
        // asked in a channel: said there, listing what anybody may use, or by notice with the asker's own list
        const bool in_channel = !c.is_pm && !reply_target.empty();
        const bool by_notice = in_channel && g_settings.get()->help_reply == "notice";
        const auto cfg = settings();
        for (const auto& line : help_text(c.args.empty() ? "" : c.args[0], cfg->cmd_prefix, current_nick(),
                                          in_channel && !by_notice ? 0 : help_level(c), esp_app_get_description()->version)) {
            if (by_notice) send_notice(sender_nick, line);
            else send_privmsg(reply_target, line);
        }
        return true;
    }

    if (cmd == "sysinfo") {
        send_privmsg(reply_target, system_info_line());
        return true;
    }

    if (cmd == "version") {
        send_privmsg(reply_target, software_description());
        return true;
    }

    // 2. STATUS / UPTIME
    if (cmd == "status" || cmd == "uptime") {
        int64_t now_sec = seconds_now();
        int64_t uptime_sec = now_sec - start_time_;
        int days = uptime_sec / 86400;
        int hours = (uptime_sec % 86400) / 3600;
        int mins = (uptime_sec % 3600) / 60;
        int secs = uptime_sec % 60;

        uint32_t free_ram = esp_get_free_heap_size() / 1024;
        std::string chs_str = "";
        for (const auto& ch : get_joined_channels()) { // the report channel stays out: anyone may ask
            if (!chs_str.empty()) chs_str += ", ";
            chs_str += ch;
        }
        char buf[384];
        std::string time_str = g_ntp_mgr.get_formatted_time();
        snprintf(buf, sizeof(buf), "PoliBOT Status (v%s): %s | Uptime: %dd %02dh %02dm %02ds | Free RAM: %u KB | Channels: [%s] | Network: %s (%s)",
                 esp_app_get_description()->version, time_str.c_str(), days, hours, mins, secs, (unsigned int)free_ram,
                 chs_str.c_str(), name_.c_str(), settings()->host.c_str());
        send_privmsg(reply_target, buf);
        return true;
    }

    if (cmd == "time" || cmd == "clock" || cmd == "date") {
        std::string time_str = g_ntp_mgr.get_formatted_time();
        std::string synced_str = g_ntp_mgr.is_synced() ? "NTP Synced" : "NTP Pending";
        std::string loc = g_ntp_mgr.get_location();
        send_privmsg(reply_target, "Local Time: " + time_str + " (" + loc + ", " + synced_str + ")");
        return true;
    }

    // 3. CHANSTATS
    if (cmd == "chanstats" || cmd == "cstat") {
        std::string ch;
        resolve_channel(c, ch);
        if (ch.empty()) {
            send_privmsg(reply_target, "Usage: chanstats <#channel>");
            return true;
        }
        ChannelGuardianConfig cfg = guardian_.get_channel_config(name_, ch);
        size_t u_cnt = 0;
        int act_lim = 0, op = -1;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(ch));
            if (it != channels_.end()) {
                u_cnt = it->second.members.size();
                act_lim = it->second.limit.current;
                op = it->second.op;
            }
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "[%s] Users: %d | Dynamic Limit: %s (+l %d) | EntryMsg: %s | State: %s | Operator: %s",
                 ch.c_str(), (int)u_cnt, cfg.dynamic_limit.enabled ? "ON" : "OFF", act_lim,
                 cfg.entrymsg.enabled ? "ON" : "OFF", cfg.lockdown ? ("LOCKED (" + g_settings.get()->lockdown_modes + ")").c_str() : "NORMAL",
                 op == 1 ? "yes" : op == 0 ? "no" : "unknown");
        send_privmsg(reply_target, buf);
        return true;
    }
    return false;
}
