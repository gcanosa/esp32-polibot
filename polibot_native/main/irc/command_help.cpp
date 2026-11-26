#include "command_help.h"
#include <algorithm>
#include <cctype>

namespace {

struct Command {
    const char* name;
    const char* aliases; // space separated, "" for none
    int level;
    const char* args;
    const char* text;
    const char* example; // arguments of the example
    bool private_only = false;
};

// Keep in step with the handlers in irc_client.cpp (the PC test checks that none is missing)
constexpr Command COMMANDS[] = {
    {"help", "", 10, "[command]", "List your commands, or explain one.", "ban"},
    {"login", "auth", 10, "[user] <password>", "Identify yourself to the bot.", "mypassword", true},
    {"logout", "", 10, "", "End your login.", ""},
    {"whoami", "", 10, "", "Show which account the bot sees you as.", ""},
    {"version", "", 10, "", "Software version and hardware.", ""},
    {"status", "uptime", 10, "", "Version, time, uptime, free memory and channels.", ""},
    {"time", "clock date", 10, "", "The bot's local time.", ""},
    {"sysinfo", "", 10, "", "The device it runs on: processor, load, memory, storage, temperature and uptime.", ""},
    {"chanstats", "cstat", 10, "[#chan]", "Users, floating limit, entry message and lockdown state.", "#polibot"},
    {"seen", "", 10, "<nick>", "When somebody was last around, and what they did.", "alice"},
    {"tell", "", 10, "<nick> <message>", "Leave a message, delivered when they are next seen.", "alice call me back"},

    {"op", "", 50, "[#chan] [nick...]", "Give operator status to the nicks named, or to yourself.", "#polibot alice bob"},
    {"deop", "", 50, "[#chan] [nick...]", "Remove operator status from the nicks named, or from yourself.", "#polibot alice"},
    {"voice", "", 50, "[#chan] [nick...]", "Give voice to the nicks named, or to yourself.", "#polibot bob carol"},
    {"devoice", "", 50, "[#chan] [nick...]", "Remove voice from the nicks named, or from yourself.", "#polibot bob"},
    {"half", "halfop hop", 50, "[#chan] [nick...]", "Give half-operator status, where the server has it, to the nicks named or to yourself.", "#polibot bob"},
    {"dehalf", "dehalfop dehop", 50, "[#chan] [nick...]", "Remove half-operator status from the nicks named, or from yourself.", "#polibot bob"},
    {"kick", "", 50, "[#chan] <nick> [reason]", "Kick a user.", "#polibot troll Please behave"},
    {"topic", "", 50, "[#chan] <text>", "Change the topic.", "#polibot Welcome!"},
    {"quiet", "", 50, "[#chan] <nick|mask> [time]", "Mute, for good or for a time such as 30m. A nick is muted by host.", "#polibot noisy 30m"},
    {"unquiet", "", 50, "[#chan] <nick|mask>", "Lift a mute.", "#polibot noisy"},

    {"ban", "", 75, "[#chan] <nick|mask> [time]", "Ban, for good or for a time such as 2h or 7d. A nick is banned by host.", "#polibot troll 2h"},
    {"unban", "", 75, "[#chan] <nick|mask>", "Remove a ban, also the ones I set against a nickname.", "#polibot *@bad.example"},
    {"akick", "", 75, "[#chan|*] list | add <nick|mask> [reason] | del <nick|mask>", "The saved list of who is banned and kicked on sight.", "#polibot add *@bad.example spam"},
    {"dynlimit", "limit", 75, "[#chan] <on [slots]|off|slots>", "Floating channel limit: +l gradually follows the user count plus free slots.", "#polibot on 3"},
    {"lockdown", "", 75, "[#chan]", "Emergency: moderated and invite only (+mi).", "#polibot"},
    {"unlock", "", 75, "[#chan]", "End a lockdown (-mi).", "#polibot"},
    {"entrymsg", "", 75, "[#chan] [message|off]", "Show, set or disable the notice sent to people who join.", "#polibot Be kind, read the rules"},
    {"badwords", "", 75, "[#chan] [list|add <word>|del <word>]", "Forbidden words of a channel.", "#polibot add spamword"},
    {"autoop", "", 75, "[#chan] [list|add <who>|del <who>]", "This channel's auto-op list. Who: a hostmask, a services account, user:<bot account>, or a nick here.", "#polibot add alice"},
    {"autohop", "autohalf autohalfop", 75, "[#chan] [list|add <who>|del <who>]", "This channel's auto half-operator list.", "#polibot add *!*@staff.example"},
    {"autovoice", "", 75, "[#chan] [list|add <who>|del <who>]", "This channel's auto-voice list.", "#polibot add user:bob"},
    {"timevoice", "timedvoice", 75, "[#chan] [on|off|<time>]", "Voice for those who have stayed a while, such as 10m, 2h or 3d.", "#polibot 30m"},
    {"join", "", 75, "<#chan> [password]", "Join a channel and keep it in the autojoin list. Give a password in private only.", "#newchan"},
    {"part", "", 75, "[#chan]", "Leave a channel and remove it from the autojoin list.", "#oldchan"},
    {"say", "", 75, "<#chan|nick> <message>", "Speak as the bot.", "#polibot Hello everyone"},
    {"rehash", "", 75, "", "Reload the saved configuration.", ""},

    {"owner", "", 100, "[#chan] [nick...]", "Give channel owner status, where the server has it. I must be an owner myself.", "#polibot alice"},
    {"deowner", "", 100, "[#chan] [nick...]", "Remove channel owner status from the nicks named, or from yourself.", "#polibot alice"},
    {"masskick", "", 100, "[#chan] <pattern> [reason]", "Kick every matching nick (50 at most). Operators and staff are spared.", "#polibot botnet* Clone flood"},
    {"admin", "", 100, "list | add <user> <password> <role> [#chan:role,...] [hostmask] [account=name] | passwd <user> <password> | del <user>",
     "Manage accounts. Changes need a login first and only work in private message.", "add alice S3cret mod account=alice", true},
    {"raw", "", 100, "<line>", "Send a line to the server as typed.", "MODE #polibot +s"},
    {"reboot", "", 100, "", "Restart the device.", ""},
    {"ota", "", 100, "[status|<url>]", "Firmware update from a link. Needs a login first, in private message.", "status", true},
};

const char* role_name(int level) {
    return level >= 100 ? "owner" : level >= 75 ? "admin" : level >= 50 ? "moderator" : "everyone";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool is_called(const Command& c, const std::string& word) {
    return word == c.name || (" " + std::string(c.aliases) + " ").find(" " + word + " ") != std::string::npos;
}

} // namespace

std::vector<std::string> help_text(const std::string& topic, const std::string& prefix, const std::string& bot_nick,
                                   int role_level, const std::string& version) {
    std::vector<std::string> lines;
    if (topic.empty()) {
        const std::string who = role_level ? std::string("your role: ") + role_name(role_level) : "staff: ask me in private for your own list";
        lines.push_back("PoliBOT v" + version + " - " + who + ". Details and an example: " + prefix +
                        "help <command> (in private message the " + prefix + " is optional)");
        for (int level : {10, 50, 75, 100}) {
            if (level > role_level && level > 10) continue;
            std::string line = level == 10 ? "General:" : level == 50 ? "Moderator:" : level == 75 ? "Admin:" : "Owner:";
            for (const auto& c : COMMANDS) {
                if (c.level == level) line += " " + prefix + c.name;
            }
            lines.push_back(line);
        }
        return lines;
    }

    // accept "help ban" as well as "help !ban"
    std::string word = lower(topic);
    if (!prefix.empty() && word.size() > prefix.size() && word.compare(0, prefix.size(), lower(prefix)) == 0) word.erase(0, prefix.size());
    for (const auto& c : COMMANDS) {
        if (!is_called(c, word)) continue;
        std::string usage = prefix + c.name;
        if (c.args[0]) usage += std::string(" ") + c.args;
        lines.push_back(usage + " - " + c.text);
        std::string example = c.private_only ? "/msg " + bot_nick + " " + c.name : prefix + c.name;
        if (c.example[0]) example += std::string(" ") + c.example;
        std::string info = "Example: " + example + "  |  For: " + role_name(c.level);
        if (c.aliases[0]) info += std::string("  |  Also: ") + c.aliases;
        lines.push_back(info);
        return lines;
    }
    lines.push_back("No command called '" + topic.substr(0, 40) + "'. Send " + prefix + "help for the list.");
    return lines;
}
