// IRCClient: feeding "seen" and delivering "tell" messages
#include "irc_client.h"
#include "irc_internal.h"
#include "bot_memory.h"

static std::string age_of(int64_t when, const Clock& now) {
    return (when && now.epoch >= when) ? format_duration(now.epoch - when) + " ago" : "a while ago";
}

void IRCClient::note_seen(const std::string& nick, const std::string& hostmask, char action, const std::string& channel, const std::string& text) {
    const auto settings = g_settings.get();
    if (!settings->seen_enabled || same(nick, current_nick()) || is_report_channel(channel)) return;
    std::string words = text;
    if (action == 'm') {
        if (words.rfind("\x01" "ACTION ", 0) == 0) {
            words = "* " + nick + " " + words.substr(8, words.find('\x01', 8) - 8);
        } else if (!words.empty() && words[0] == '\x01') {
            return; // other CTCP requests are not something somebody said
        }
        // never keep anything that may hold a password
        if (!settings->seen_text || redact_command(words) != words) words.clear();
    }
    g_memory.seen.note(name_, nick, user_host(hostmask), action, channel, words, bot_clock(), settings->seen_max);
}

void IRCClient::deliver_tells(const std::string& nick, const std::string& channel) {
    const auto settings = g_settings.get();
    if (!settings->tell_enabled || same(nick, current_nick())) return;
    const Clock now = bot_clock();
    for (const auto& message : g_memory.mail.take(name_, nick)) {
        const std::string line = std::string(message.from) + " left you a message " + age_of(message.when, now) + ": " + message.text;
        if (settings->tell_delivery == "channel" && !channel.empty()) send_privmsg(channel, nick + ": " + line);
        else if (settings->tell_delivery == "notice") send_notice(nick, line);
        else send_privmsg(nick, line);
    }
}
