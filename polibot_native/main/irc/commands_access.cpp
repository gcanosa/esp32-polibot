// autoop, autohop, autovoice: a channel's lists; timevoice: voice after a while
#include "irc_client.h"
#include "irc_internal.h"
#include "access_list.h"

bool IRCClient::access_command(Command& c) {
    const std::string& cmd = c.name;
    const char mode = cmd == "autoop" ? 'o'
                    : cmd == "autohop" || cmd == "autohalf" || cmd == "autohalfop" ? 'h'
                    : cmd == "autovoice" ? 'v' : 0;
    if (!mode) return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    const std::string sub = c.args.empty() ? "list" : irc_lower(c.args[0]);
    if (!is_channel_name(ch) || (sub != "list" && ((sub != "add" && sub != "del") || c.args.size() != 2))) {
        send_privmsg(c.reply_to, "Usage: " + cmd + " [#chan] [list|add <who>|del <who>]  (who: hostmask, services account, user:<bot account> or a nick here)");
        return true;
    }
    const std::string list = std::string(mode == 'o' ? "auto-op" : mode == 'h' ? "auto half-op" : "auto-voice") + " list of " + ch;
    if (sub == "list") {
        send_privmsg(c.reply_to, "The " + list + ": " + list_line(guardian_.access_entries(name_, ch, mode)));
        return true;
    }
    const std::string& typed = c.args[1];
    std::string who = access_who(typed);
    if (sub == "add") {
        const std::string problem = guardian_.add_access(name_, ch, mode, who);
        if (!problem.empty()) {
            send_privmsg(c.reply_to, problem);
            return true;
        }
    } else if (guardian_.remove_access(name_, ch, mode, access_mask(typed))) { // as typed
        who = access_mask(typed);
    } else if (!guardian_.remove_access(name_, ch, mode, who)) { // else what the nickname stands for
        send_privmsg(c.reply_to, typed + " is not on the " + list + ".");
        return true;
    }
    c.audit = true;
    review_modes(ch);
    send_privmsg(c.reply_to, (sub == "add" ? "Added " + who + " to the " : "Removed " + who + " from the ") + list + ".");
    return true;
}

bool IRCClient::timevoice_command(Command& c) {
    if (c.name != "timevoice" && c.name != "timedvoice") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    const std::string usage = "Usage: timevoice [#chan] [on|off|<time, e.g. 10m, 2h, 3d>]";
    if (!is_channel_name(ch) || c.args.size() > 1) {
        send_privmsg(c.reply_to, usage);
        return true;
    }
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, ch);
    TimedVoiceConfig& voice = rules.timed_voice;
    if (!c.args.empty()) {
        const std::string value = irc_lower(c.args[0]);
        int seconds = 0;
        if (value == "on" || value == "off") {
            voice.enabled = value == "on";
        } else if (parse_duration(value, seconds) && seconds >= 60) {
            voice.enabled = true;
            voice.minutes = (seconds + 59) / 60;
        } else {
            send_privmsg(c.reply_to, usage + "  (a minute at least)");
            return true;
        }
        guardian_.save_channel_config(name_, ch, rules);
        review_modes(ch);
        c.audit = true;
    }
    send_privmsg(c.reply_to, ch + " timed voice is " + (voice.enabled ? "ON: after " + format_duration((int64_t)voice.minutes * 60) + " in the channel." : "OFF."));
    return true;
}
