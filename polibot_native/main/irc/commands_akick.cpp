// akick: the saved auto-kick list
#include "irc_client.h"
#include "irc_internal.h"
#include "bot_memory.h"

bool IRCClient::akick_command(Command& c) {
    if (c.name != "akick") return false;
    std::vector<std::string>& args = c.args;

    // "*" or nothing in private means every channel
    std::string ch;
    if (!args.empty() && (args[0] == "*" || args[0][0] == '#')) {
        if (args[0] != "*") ch = args[0];
        args.erase(args.begin());
    } else if (!c.is_pm && !c.target.empty() && c.target[0] == '#') {
        ch = c.target;
    }
    if (!allowed(c, "admin", ch)) return true; // every channel needs a global administrator
    const std::string sub = args.empty() ? "list" : irc_lower(args[0]);
    const std::string where = ch.empty() ? "all channels" : ch;

    if (sub == "list") {
        std::vector<std::string> entries;
        for (const auto& a : guardian_.get_akicks()) {
            if (!a.network.empty() && !same(a.network, name_)) continue;
            if (!ch.empty() && !a.channel.empty() && !same(a.channel, ch)) continue;
            entries.push_back(a.mask + " [" + (a.channel.empty() ? "all" : a.channel) + "]" + (a.reason.empty() ? "" : " " + a.reason));
        }
        send_privmsg(c.reply_to, "Auto-kick list: " + list_line(entries));
        return true;
    }
    if ((sub != "add" && sub != "del") || args.size() < 2) {
        send_privmsg(c.reply_to, "Usage: akick [#chan|*] list | add <nick|mask> [reason] | del <nick|mask>");
        return true;
    }
    const std::vector<std::string> channels = ch.empty() ? get_joined_channels() : std::vector<std::string>{ch};

    if (sub == "add") {
        const std::string mask = mask_for(args[1]);
        const std::string mine = current_nick() + "!" + member_userhost(current_nick());
        if (!mask_is_specific(mask) || wildcard_match(mask, mine)) {
            send_privmsg(c.reply_to, "Refused: " + mask + " would match everybody, or me.");
            return true;
        }
        if (!guardian_.add_akick({name_, ch, mask, args.size() > 2 ? join_words(args, 2) : ""})) {
            send_privmsg(c.reply_to, "Could not add it: the list is full or could not be saved.");
            return true;
        }
        send_privmsg(c.reply_to, "Added " + mask + " to the auto-kick list of " + where + ".");
        c.audit = true;
        for (const auto& channel : channels) sweep_akicks(channel);
        return true;
    }

    // the mask as typed, else the one this nickname stands for
    std::string mask = args[1];
    if (!guardian_.remove_akick(name_, ch, mask)) {
        mask = mask_for(args[1]);
        if (!guardian_.remove_akick(name_, ch, mask)) {
            send_privmsg(c.reply_to, args[1] + " is not on the auto-kick list of " + where + ".");
            return true;
        }
    }
    for (const auto& channel : channels) { // and lift the bans it caused
        if (g_memory.bans.remove(name_, channel, "+b " + mask)) send_raw("MODE " + channel + " -b " + mask);
    }
    send_privmsg(c.reply_to, "Removed " + mask + " from the auto-kick list of " + where + ".");
    c.audit = true;
    return true;
}
