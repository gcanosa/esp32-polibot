#include "outbox.h"
#include "irc_text.h"

LinePriority line_priority(const std::string& line, bool registered) {
    const std::string word = irc_lower(line.substr(0, line.find_first_of(" \r")));
    if (word == "pong" || word == "ping" || word == "quit" || word == "cap" || word == "authenticate" || word == "pass" ||
        word == "user" || (word == "nick" && !registered)) return LinePriority::urgent;
    if (word == "kick" || word == "mode" || word == "join" || word == "part" || word == "invite" || word == "nick") return LinePriority::action;
    const std::string start = irc_lower(line.substr(0, 17));
    return start == "privmsg chanserv " || start == "privmsg nickserv " ? LinePriority::action : LinePriority::talk;
}

Outbox::Offer Outbox::offer(const std::string& line, LinePriority priority, int64_t now_ms, int burst, int interval_ms) {
    const bool action = priority == LinePriority::action;
    const bool nothing_ahead = actions_.empty() && (action || talk_.empty());
    if (nothing_ahead && bucket_.take(now_ms, burst, interval_ms)) return Offer::send_now;
    return (action ? actions_ : talk_).push(line) ? Offer::queued : Offer::dropped;
}

bool Outbox::next(std::string& line, int64_t now_ms, int burst, int interval_ms) {
    if (!pending() || !bucket_.take(now_ms, burst, interval_ms)) return false;
    return (actions_.empty() ? talk_ : actions_).pop(line);
}

void Outbox::clear() {
    actions_.clear();
    talk_.clear();
    bucket_.reset();
}
