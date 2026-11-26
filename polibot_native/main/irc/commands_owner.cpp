// masskick, admin, ota, reboot, raw
#include "irc_client.h"
#include "irc_internal.h"
#include "ota_manager.h"
#include <esp_system.h>
#include <sstream>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

bool IRCClient::masskick_command(Command& c) {
    if (c.name != "masskick") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "owner")) return true;
    if (ch.empty() || c.args.empty()) {
        send_privmsg(c.reply_to, "Usage: masskick [#chan] <nick pattern> [reason]");
        return true;
    }
    const std::string reason = c.args.size() > 1 ? join_words(c.args, 1) : "Mass kick sweep";
    bool joined = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        joined = channels_.count(irc_lower(ch)) > 0;
        if (joined) masskick_ = {true, ch, c.args[0], reason, c.reply_to, {}, 0};
    }
    if (!joined) {
        send_privmsg(c.reply_to, "I am not in " + ch + ".");
        return true;
    }
    c.audit = true;
    send_raw("WHO " + ch); // finish_masskick() runs when the list is complete
    return true;
}

// What may follow "admin add <user> <password> <role>": "#chan:role,net/#other:role", hostmasks, and
// "account=name" (this network) or "account=network/name" for services accounts
static bool parse_account_extras(const std::vector<std::string>& args, size_t from, const std::string& network,
                                 std::map<std::string, std::string>& roles, std::vector<std::string>& masks,
                                 std::vector<std::string>& accounts) {
    for (size_t i = from; i < args.size(); i++) {
        if (args[i].rfind("account=", 0) == 0) {
            const std::string account = args[i].substr(8);
            accounts.push_back(account.find('/') == std::string::npos ? network + "/" + account : account);
            continue;
        }
        if (args[i].find_first_of("!@") != std::string::npos) {
            masks.push_back(args[i]);
            continue;
        }
        std::istringstream items(args[i]);
        std::string item;
        while (std::getline(items, item, ',')) {
            const size_t colon = item.rfind(':');
            if (colon == std::string::npos || colon == 0) return false;
            roles[irc_lower(item.substr(0, colon))] = irc_lower(item.substr(colon + 1));
        }
    }
    return true;
}

bool IRCClient::admin_command(Command& c) {
    if (c.name != "admin") return false;
    if (!allowed(c, "owner")) return true;
    std::vector<std::string>& args = c.args;
    const std::string sub = args.empty() ? "list" : irc_lower(args[0]);
    const std::string usage = "Usage: admin list | add <user> <password> <role> [#chan:role,...] [hostmask] [account=name] | passwd <user> <password> | del <user>";
    if (sub == "list") {
        std::vector<std::string> accounts;
        for (const auto& u : security_mgr_.list_users()) {
            std::string entry = u.username + " [" + u.global_role;
            for (const auto& r : u.channel_roles) entry += " " + r.first + ":" + r.second;
            for (const auto& a : u.accounts) entry += " account=" + a;
            accounts.push_back(entry + "]");
        }
        send_privmsg(c.reply_to, "Accounts: " + list_line(accounts));
        return true;
    }
    // These carry passwords and hand out control of the device
    if (c.hostmask != WEB_CONSOLE_HOSTMASK && !(c.is_pm && security_mgr_.has_owner_session(name_, c.nick, c.hostmask))) {
        send_privmsg(c.reply_to, "Permission denied: PM me 'login <password>' first, then repeat this in PM. Never type passwords in a channel.");
        return true;
    }
    std::pair<bool, std::string> result{false, usage};
    if (sub == "del" && args.size() == 2) {
        result.first = security_mgr_.remove_user(args[1], result.second);
    } else if (sub == "passwd" && args.size() == 3) {
        result = security_mgr_.set_password(args[1], args[2]);
    } else if (sub == "add" && args.size() >= 4) {
        std::map<std::string, std::string> roles;
        std::vector<std::string> masks, services;
        if (parse_account_extras(args, 4, name_, roles, masks, services)) {
            // without channel roles or "account=" an existing user keeps what they have
            result = security_mgr_.add_or_update_user(args[1], args[2], irc_lower(args[3]), roles.empty() ? nullptr : &roles, masks, "",
                                                      services.empty() ? nullptr : &services);
        }
    }
    c.audit = result.first;
    send_privmsg(c.reply_to, result.second);
    return true;
}

bool IRCClient::ota_command(Command& c) {
    if (c.name != "ota") return false;
    // Flashing firmware = full device control, so hostmask matching alone is not enough
    if (!c.is_pm || !security_mgr_.has_owner_session(name_, c.nick, c.hostmask)) {
        send_privmsg(c.reply_to, "Permission denied (Owner only: PM me 'login <password>' first, then 'ota <url>').");
        return true;
    }
    if (c.args.empty() || c.args[0] == "status") {
        OtaStatus s = g_ota_mgr.get_status();
        send_privmsg(c.reply_to, "OTA: " + s.status_message + " | Running " + s.running_partition +
                     " (v" + s.app_version + ", " + s.compile_time + ")");
        return true;
    }
    send_privmsg(c.reply_to, "OTA starting...");
    // keeps this connection object alive even if the network is removed during the download
    const bool started = g_ota_mgr.start_url_update(c.args[0], [self = shared_from_this(), to = c.reply_to](bool, const std::string& msg) {
        self->send_privmsg(to, msg);
    });
    c.audit = started;
    if (started) send_privmsg(c.reply_to, "Download running; 'ota status' shows progress. Reboots automatically on success.");
    return true;
}

bool IRCClient::owner_command(Command& c) {
    if (masskick_command(c) || admin_command(c) || ota_command(c)) return true;
    const std::string& cmd = c.name;

    if (cmd == "reboot") {
        if (!allowed(c, "owner")) return true;
        send_privmsg(c.reply_to, "Rebooting ESP32-S3 Sentry now...");
        g_irc_manager.report(ReportKind::system, c.nick + " asked for a restart.");
        for (auto& client : g_irc_manager.clients()) client->flush_reports(); // there is no later
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return true;
    }

    if (cmd == "raw") {
        if (!allowed(c, "owner")) return true;
        const std::string raw_line = join_words(c.args);
        c.audit = true;
        send_raw(raw_line);
        send_privmsg(c.reply_to, "Sent raw: " + redact_command(raw_line));
        return true;
    }
    return false;
}
