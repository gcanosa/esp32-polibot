// IRCClient: what each line from the server leads to
#include "irc_client.h"
#include "irc_internal.h"
#include <algorithm>
#include <sstream>
#include <cstring>

static void parse_user_prefix(const std::string& prefix, std::string& nick, std::string& user, std::string& host) {
    nick = prefix;
    user = "";
    host = "";
    size_t ex = nick.find('!');
    if (ex != std::string::npos) {
        std::string rest = nick.substr(ex + 1);
        nick = nick.substr(0, ex);
        size_t at = rest.find('@');
        if (at != std::string::npos) {
            user = rest.substr(0, at);
            host = rest.substr(at + 1);
        } else {
            user = rest;
        }
    }
}

void IRCClient::process_raw_line(const std::string& raw_line) {
    std::string line = raw_line;
    std::string prefix, command, trailing;
    std::vector<std::string> args;

    if (line.empty()) return;

    std::string tags; // IRCv3, only those of the capabilities asked for
    if (line[0] == '@') {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) return;
        tags = line.substr(1, sp - 1);
        line.erase(0, sp + 1);
        if (line.empty()) return;
    }

    if (line[0] == ':') {
        size_t sp = line.find(' ');
        if (sp != std::string::npos) {
            prefix = line.substr(1, sp - 1);
            line = line.substr(sp + 1);
        }
    }

    size_t col = line.find(" :");
    if (col != std::string::npos) {
        trailing = line.substr(col + 2);
        line = line.substr(0, col);
    }

    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        args.push_back(token);
    }
    if (args.empty()) return;

    command = args[0];
    std::transform(command.begin(), command.end(), command.begin(), ::toupper);
    args.erase(args.begin());

    std::string sender_nick, user, host;
    parse_user_prefix(prefix, sender_nick, user, host);
    std::string hostmask = prefix;

    const auto shared = settings();
    const IrcSettings& cfg = *shared;
    const std::string me = current_nick(); // what the server knows us as, which may lag behind the settings
    const bool from_me = same(sender_nick, me);

    // 1. PING
    if (command == "PING") {
        std::string payload = trailing.empty() ? (args.empty() ? "" : args[0]) : trailing;
        send_raw("PONG :" + payload);
        return;
    }

    if (!registered_ && capability_command(command, args, trailing)) return;

    // before anything is judged: who is the sender signed in as?
    if (cap_account_tag_ && prefix.find('!') != std::string::npos && (command == "PRIVMSG" || command == "NOTICE" || command == "JOIN")) {
        // on a join the modes are handled once they are in the channel
        if (learn_account(sender_nick, user_host(hostmask), tag_value(tags, "account")) && command != "JOIN") grant_everywhere(sender_nick);
    }

    // 2. RPL_WELCOME (001)
    if (command == "001") {
        registered_ = true;
        if (!args.empty()) { // the server may have shortened it
            std::lock_guard<std::mutex> lock(state_mutex_);
            current_nick_ = args[0];
        }
        cap_deadline_ = 0; // a server that knows nothing of CAP welcomes right away
        if (!cfg.nickserv_pw.empty() && !sasl_ok_) {
            log("Identifying with NickServ...");
            // under a replacement nickname the account has to be named
            const bool own_nick = args.empty() || same(args[0], cfg.nick);
            const std::string account = !cfg.account.empty() ? cfg.account : own_nick ? "" : cfg.nick;
            send_raw("PRIVMSG NickServ :IDENTIFY " + (account.empty() ? "" : account + " ") + cfg.nickserv_pw);
            pause_ms(2000); // channels that require a registered nickname need services to answer first
        }
        run_perform(cfg);
        log("Joined network successfully. Autojoining channels...");
        for (const auto& ch : cfg.channels) {
            send_raw(join_line(ch));
        }
        if (!cfg.report_channel.empty()) send_raw(join_line(cfg.report_channel));
        return;
    }

    // 432 invalid, 433 in use, 437 unavailable, 438 changing too fast
    if (command == "432" || command == "433" || command == "437" || command == "438") {
        if (args.size() >= 2 && args[1][0] == '#') { // 437 is also used for channels
            channel_event(command, args, trailing, sender_nick, hostmask, from_me);
            return;
        }
        const std::string wanted = args.size() >= 2 ? args[1] : cfg.nick;
        if (!registered_ && command != "438") {
            try_another_nick(cfg, wanted);
        } else {
            log("Cannot use nickname '" + wanted + "': " + trailing + (registered_ ? " (still " + me + ")" : ""));
        }
        return;
    }

    // Bot mode: who we are told we can be (004, 005), when to ask (end of the welcome) and the answer
    if (command == "004" && args.size() > 3) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        bot_listed_ = args[3].find('B') != std::string::npos;
        return;
    }
    if (command == "376" || command == "422") { // end of the message of the day, or none
        if (cfg.bot_mode) apply_bot_mode();
        return;
    }
    if (command == "221" || (command == "MODE" && !args.empty() && same(args[0], me))) { // our own user modes
        on_user_modes(args.size() > 1 ? args[1] : trailing);
        return;
    }
    if (command == "501") { // unknown user mode
        on_bot_mode_refused();
        return;
    }

    // Whoever held the configured nickname just left or renamed: take it back
    if (registered_ && !from_me && (command == "QUIT" || command == "NICK") &&
        same(sender_nick, cfg.nick) && !same(me, cfg.nick)) {
        log("'" + cfg.nick + "' is free again. Taking it back.");
        send_raw("NICK " + cfg.nick);
    }

    if (command == "PRIVMSG" || command == "NOTICE") {
        if (args.empty() || prefix.find('!') == std::string::npos) return; // the server itself says nothing to act on
        on_message(command, args[0], sender_nick, hostmask, trailing.empty() && args.size() > 1 ? args[1] : trailing);
        return;
    }

    channel_event(command, args, trailing, sender_nick, hostmask, from_me);
}

void IRCClient::on_message(const std::string& command, const std::string& to, const std::string& nick, const std::string& hostmask,
                           const std::string& message) {
    const auto shared = settings();
    const IrcSettings& cfg = *shared;
    const bool from_me = same(nick, current_nick());
    // "@#chan" and "+#chan" address part of a channel: still that channel
    std::string target = to;
    while (target.size() > 1 && strchr("~&@%+", target[0]) && strchr("#&~@%+", target[1])) target.erase(0, 1);
    const bool in_channel = !target.empty() && (target[0] == '#' || target[0] == '&');

    if (command == "NOTICE") { // never a command, but channel notices follow the channel rules
        if (in_channel && !from_me) moderate(target, nick, hostmask, message);
        return;
    }

    // CTCP requests are answered with a NOTICE and are never bot commands
    if (!message.empty() && message[0] == '\x01') {
        if (message.rfind("\x01VERSION", 0) == 0) {
            const int64_t now_sec = seconds_now();
            if (now_sec - last_ctcp_reply_ >= 3) { // stay silent rather than be flooded off the server
                last_ctcp_reply_ = now_sec;
                send_notice(nick, "\x01VERSION " + software_description() + "\x01");
            }
        }
        if (!in_channel) return;
    }

    std::string cmd_body = "";
    if (message.rfind(cfg.cmd_prefix, 0) == 0) {
        cmd_body = message.substr(cfg.cmd_prefix.length());
    } else if (!in_channel) {
        cmd_body = message;
    }

    if (!from_me) {
        // the text of a command is not kept: it may hold a password
        if (in_channel) note_seen(nick, hostmask, 'm', target, cmd_body.empty() ? message : "");
        deliver_tells(nick, in_channel ? target : "");
        // every channel line follows the rules, commands too; a line that broke one is not obeyed
        if (in_channel && moderate(target, nick, hostmask, message)) return;
    }

    if (!cmd_body.empty()) handle_command(target, nick, hostmask, cmd_body, !in_channel);
}
