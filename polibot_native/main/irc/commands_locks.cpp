// topiclock, modelock
#include "irc_client.h"
#include "mode_lock.h"

bool IRCClient::lock_command(Command& c) {
    const std::string& cmd = c.name;
    if (cmd != "topiclock" && cmd != "modelock") return false;
    std::string ch;
    resolve_channel(c, ch);
    if (!allowed(c, "admin", ch)) return true;
    const bool topic = cmd == "topiclock";
    if (ch.empty() || is_report_channel(ch)) {
        send_privmsg(c.reply_to, topic ? "Usage: topiclock <#chan> [on|off|<topic>]" : "Usage: modelock <#chan> [<modes>|off]");
        return true;
    }
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, ch);
    if (c.args.empty()) {
        send_privmsg(c.reply_to, topic ? ch + " Topic Lock: " + (rules.topic_lock ? rules.topic : "OFF")
                                       : ch + " Mode Lock: " + (rules.mode_lock.empty() ? "OFF" : rules.mode_lock));
        return true;
    }
    const std::string first = irc_lower(c.args[0]);
    const bool off = c.args.size() == 1 && (first == "off" || first == "disable");
    if (topic) {
        rules.topic_lock = !off;
        if (c.args.size() == 1 && (first == "on" || first == "enable")) {
            std::vector<std::string> joined = get_joined_channels();
            if (!contains_ci(joined, ch)) {
                send_privmsg(c.reply_to, "I am not in " + ch + ", so I do not know its topic: give it.");
                return true;
            }
            rules.topic = current_topic(ch);
        } else if (!off) {
            rules.topic = join_words(c.args);
        }
    } else {
        const std::string modes = off ? "" : clean_mode_lock(join_words(c.args));
        if (!off && modes.empty()) {
            send_privmsg(c.reply_to, "Only plain channel modes can be locked, such as +nt-s (no ranks, lists, keys or limits).");
            return true;
        }
        rules.mode_lock = modes;
    }
    guardian_.save_channel_config(name_, ch, rules);
    enforce_locks(ch);
    c.audit = true;
    send_privmsg(c.reply_to, off ? std::string(topic ? "Topic" : "Mode") + " lock of " + ch + " is off."
                             : topic ? "Topic of " + ch + " locked: " + rules.topic : "Modes of " + ch + " locked: " + rules.mode_lock);
    return true;
}
