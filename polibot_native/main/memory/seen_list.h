#pragma once

// Who was around, for !seen. A bounded list; the one quiet for longest is forgotten first.
#include <cstdint>
#include <mutex>
#include <string>
#include "bot_clock.h"
#include "psram_alloc.h"

struct SeenRecord {
    char network[33];
    char nick[32];
    char userhost[96];
    char channel[65];
    char text[101]; // what was said, the quit reason, who kicked, the new nickname
    char action;    // 'm' said, 'j' joined, 'p' left, 'q' quit, 'k' was kicked, 'n' changed nickname
    int64_t when;   // wall clock, 0 = unknown
    uint32_t order; // newest is highest
};

class SeenList {
public:
    // An empty userhost keeps the one already known
    void note(const std::string& network, const std::string& nick, const std::string& userhost, char action,
              const std::string& channel, const std::string& text, const Clock& now, size_t max_entries);
    bool find(const std::string& network, const std::string& nick, SeenRecord& out);
    void forget_older_than(const Clock& now, int days);

    void load(const std::string& path, const Clock& now);
    // This list changes constantly, so it is written at most every `interval` seconds unless forced
    bool save(const std::string& path, const Clock& now, int interval, bool force);

private:
    std::mutex mutex_;
    PsramVector<SeenRecord> records_;
    uint32_t order_ = 0;
    bool changed_ = false;
    int64_t saved_at_ = 0;
};
