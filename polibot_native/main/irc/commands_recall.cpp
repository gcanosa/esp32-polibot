// seen, tell
#include "irc_client.h"
#include "irc_internal.h"
#include "bot_memory.h"

// "alice was last seen 3h 5m ago, joining #chan". The place is left out when it is not the asker's to know.
static std::string describe(const std::string& nick, const SeenRecord& record, const std::string& age, bool known_place) {
    if (!known_place) return nick + " was last seen " + age + " in another channel.";
    const std::string channel = record.channel, text = record.text;
    const std::string quoted = text.empty() ? "" : ": " + text;
    const std::string doing = record.action == 'j' ? "joining " + channel
                            : record.action == 'p' ? "leaving " + channel + quoted
                            : record.action == 'q' ? "quitting" + quoted
                            : record.action == 'k' ? "being kicked from " + channel + " by " + text
                            : record.action == 'n' ? "changing nickname to " + text
                            : text.empty()         ? "talking in " + channel
                                                   : "saying in " + channel + quoted;
    return nick + " was last seen " + age + ", " + doing;
}

bool IRCClient::seen_command(Command& c) {
    if (c.name != "seen") return false;
    if (!g_settings.get()->seen_enabled) {
        send_privmsg(c.reply_to, "Seen is switched off.");
        return true;
    }
    if (c.args.empty()) {
        send_privmsg(c.reply_to, "Usage: seen <nick>");
        return true;
    }
    const std::string nick = c.args[0].substr(0, 31);
    if (same(nick, c.nick)) {
        send_privmsg(c.reply_to, "You are right here.");
        return true;
    }
    if (same(nick, current_nick())) {
        send_privmsg(c.reply_to, "I am right here.");
        return true;
    }
    auto present = [&](const std::string& channel, const std::string& who) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        return it != channels_.end() && it->second.members.find(who) != nullptr;
    };
    // channels the asker may hear about: the one asked in and those they are in themselves
    const bool staff = help_level(c) >= get_role_level("mod");
    auto visible = [&](const std::string& channel) {
        return !is_report_channel(channel) && (staff || same(channel, c.target) || present(channel, c.nick));
    };

    std::string here;
    for (const auto& channel : get_joined_channels()) {
        if (present(channel, nick) && visible(channel)) {
            here = channel;
            break;
        }
    }
    SeenRecord record;
    const bool found = g_memory.seen.find(name_, nick, record);
    const Clock now = bot_clock();
    const std::string age = found && record.when && now.epoch >= record.when ? format_duration(now.epoch - record.when) + " ago" : "a while ago";
    const bool known_place = found && record.channel[0] && visible(record.channel);

    std::string answer;
    if (!here.empty()) {
        answer = nick + " is in " + here + " right now.";
        if (known_place && record.action == 'm' && record.text[0]) answer += " Last said " + age + " in " + record.channel + ": " + record.text;
    } else {
        answer = found ? describe(nick, record, age, known_place) : "I have not seen " + nick + ".";
    }
    send_privmsg(c.reply_to, answer);
    return true;
}

bool IRCClient::tell_command(Command& c) {
    if (c.name != "tell") return false;
    const auto settings_now = g_settings.get();
    if (!settings_now->tell_enabled) {
        send_privmsg(c.reply_to, "Tell is switched off.");
        return true;
    }
    if (c.args.size() < 2) {
        send_privmsg(c.reply_to, "Usage: tell <nick> <message>");
        return true;
    }
    std::string to = c.args[0];
    while (!to.empty() && (to.back() == ':' || to.back() == ',')) to.pop_back();
    if (to.empty() || to.size() > 31 || to.find_first_of("#&!@*?") != std::string::npos) {
        send_privmsg(c.reply_to, "That is not a nickname. Usage: tell <nick> <message>");
        return true;
    }
    if (same(to, c.nick)) {
        send_privmsg(c.reply_to, "You can tell yourself now.");
        return true;
    }
    if (same(to, current_nick())) {
        send_privmsg(c.reply_to, "I am listening: no need to leave me a message.");
        return true;
    }
    const TellResult result = g_memory.mail.add(name_, to, c.nick, join_words(c.args, 1), bot_clock(), settings_now->tell_per_recipient,
                                                settings_now->tell_per_sender, settings_now->tell_max);
    send_privmsg(c.reply_to, result == TellResult::stored ? "I will tell " + to + " when I see them."
                             : result == TellResult::mailbox_full ? to + " already has as many messages waiting as I keep."
                             : result == TellResult::sender_full ? "You already have too many messages waiting for delivery."
                                                                 : "I cannot keep any more messages for now.");
    return true;
}
