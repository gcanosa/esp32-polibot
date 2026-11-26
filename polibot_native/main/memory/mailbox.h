#pragma once

// Messages waiting for someone, for !tell
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "bot_clock.h"
#include "psram_alloc.h"

struct TellRecord {
    char network[33];
    char to[32];
    char from[32];
    char text[301];
    int64_t when; // wall clock, 0 = unknown
};

enum class TellResult { stored, mailbox_full, sender_full, full };

class Mailbox {
public:
    TellResult add(const std::string& network, const std::string& to, const std::string& from, const std::string& text,
                   const Clock& now, int per_recipient, int per_sender, int max_total);
    std::vector<TellRecord> take(const std::string& network, const std::string& nick); // removes them
    void forget_older_than(const Clock& now, int days);

    void load(const std::string& path);
    bool save(const std::string& path); // only if something changed

private:
    std::mutex mutex_;
    PsramVector<TellRecord> records_;
    bool changed_ = false;
};
