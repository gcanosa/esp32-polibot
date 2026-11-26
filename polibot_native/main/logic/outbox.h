#pragma once

// Paces what the bot sends, so that the server does not disconnect it for flooding: a burst, then
// one line per interval. Actions on channels go before talk.
#include <cstdint>
#include <string>
#include "rate_limits.h"
#include "line_queue.h"

// urgent: the connection cannot wait for it; action: acts on a channel or asks a service
enum class LinePriority { urgent, action, talk };

LinePriority line_priority(const std::string& line, bool registered);

class Outbox {
public:
    enum class Offer { send_now, queued, dropped };

    // What to do with a line that is not urgent
    Offer offer(const std::string& line, LinePriority priority, int64_t now_ms, int burst, int interval_ms);
    // The next waiting line whose turn has come
    bool next(std::string& line, int64_t now_ms, int burst, int interval_ms);
    bool pending() const { return !actions_.empty() || !talk_.empty(); }
    void clear();

private:
    TokenBucket bucket_;
    LineQueue actions_{8192}; // a full masskick fits
    LineQueue talk_{6144};
};
