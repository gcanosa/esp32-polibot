#pragma once

// The bans and mutes the bot placed: remembered so they can be lifted on time, or by nickname
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "bot_clock.h"
#include "psram_alloc.h"
#include "record_file.h"

struct BanRecord {
    char network[33];
    char channel[65];
    char mode[128];   // how it was set, e.g. "+b *!*@host"
    char nick[32];    // who it was aimed at, empty for a mask
    int64_t deadline; // steady clock; 0 = permanent; -1 = waiting for the wall clock
    int64_t until;    // wall clock end, used while deadline is -1
};

class BanList {
public:
    static constexpr size_t MAX = 200;

    // seconds = 0 for a permanent one. A record with the same network, channel and mode is replaced.
    void add(const std::string& network, const std::string& channel, const std::string& mode, const std::string& nick,
             int seconds, const Clock& now);
    bool remove(const std::string& network, const std::string& channel, const std::string& mode);
    // The modes set against a nickname in a channel
    std::vector<std::string> for_nick(const std::string& network, const std::string& channel, const std::string& nick);
    std::vector<BanRecord> expired(const std::string& network, const Clock& now); // does not remove them
    // True if `matches` accepts one of the modes set in this channel ("+b *!*@host"); nothing is copied
    template <typename F>
    bool any_of(const std::string& network, const std::string& channel, F matches) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& r : records_) {
            if (record::is(r.channel, channel) && record::is(r.network, network) && matches(r.mode)) return true;
        }
        return false;
    }

    void load(const std::string& path, const Clock& now);
    bool save(const std::string& path, const Clock& now); // only if something changed

private:
    std::mutex mutex_;
    PsramVector<BanRecord> records_;
    bool changed_ = false;
};
