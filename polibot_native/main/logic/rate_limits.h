#pragma once

// Rate limits and raid detection. Every table has a fixed size and keeps hashes instead of
// strings, so a flood can never make the bot run out of memory: the oldest entries fall out.
// Times are seconds on any steady clock; the token bucket counts milliseconds.
#include <cstdint>
#include <string>
#include <vector>
#include "psram_alloc.h"

uint32_t key_hash(const std::string& key); // never 0

// `burst` lines at once, then one every `interval_ms`
class TokenBucket {
public:
    bool take(int64_t now_ms, int burst, int interval_ms);
    void reset() { last_ms_ = -1; }

private:
    int64_t credit_ms_ = 0;
    int64_t last_ms_ = -1;
};

// Counts events per key inside a sliding window
class RateWindow {
public:
    explicit RateWindow(size_t capacity) : capacity_(capacity) {}
    // Records an event; returns how many this key had within the last `window` seconds
    int hit(const std::string& key, int64_t now, int window);
    void clear(const std::string& key);
    void clear() { events_.clear(); next_ = 0; }

private:
    struct Event {
        uint32_t key, when;
    };
    PsramVector<Event> events_; // filled up to capacity_, then reused in a circle
    size_t capacity_;
    size_t next_ = 0;
};

// Keys remembered until a given time, such as the users lost in a netsplit
class ExpiringSet {
public:
    explicit ExpiringSet(size_t capacity) : capacity_(capacity) {}
    void add(const std::string& key, int64_t now, int64_t until);
    int64_t remaining(const std::string& key, int64_t now) const; // seconds left, 0 = not there
    void clear();

private:
    struct Entry {
        uint32_t key, until;
    };
    PsramVector<Entry> entries_;
    size_t capacity_;
};

// Failed password attempts per source
class LoginGuard {
public:
    static constexpr int MAX_ATTEMPTS = 50; // the most that can be counted
    int blocked_for(const std::string& source, int64_t now) const { return (int)blocked_.remaining(source, now); }
    // True if this failure started a block
    bool failed(const std::string& source, int64_t now, int max_attempts, int window, int block);

private:
    RateWindow failures_{128};
    ExpiringSet blocked_{32};
};

// The same text from several different users within a short time
class CloneDetector {
public:
    // Once `users` different user@hosts sent this text within `window`, returns the nicks not yet
    // returned for it (all of them the first time, then each newcomer). Otherwise nothing.
    std::vector<std::string> add(const std::string& channel, const std::string& userhost, const std::string& nick,
                                 const std::string& text, int64_t now, int users, int window);

private:
    static constexpr size_t LINES = 64;
    struct Line {
        uint32_t text, user, when;
        bool returned;
        char nick[32];
    };
    PsramVector<Line> lines_;
    size_t next_ = 0;
};

// Offences per user, for escalation
class Strikes {
public:
    // 1 for a first offence, 2 for the next one within `reset` seconds, and so on
    int add(const std::string& key, int64_t now, int reset);

private:
    static constexpr size_t ENTRIES = 64;
    struct Entry {
        uint32_t who, last;
        int count;
    };
    PsramVector<Entry> entries_;
};
