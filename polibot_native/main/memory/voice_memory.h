#pragma once

// Who has earned timed voice in a channel, and who had voice taken away. Only a hash of network,
// channel and person is kept: 16 bytes each, in external RAM.
#include <cstdint>
#include <mutex>
#include <string>
#include "bot_clock.h"
#include "psram_alloc.h"

enum class VoiceState { unknown, earned, revoked };

class VoiceMemory {
public:
    // A person is known by user@host and, when signed in to services, by account. Both are looked up,
    // so that signing out does not undo a loss of voice. A loss outweighs what was earned.
    VoiceState find(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account);
    // Kept under the account if there is one; asking again makes the entry recent
    void earn(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account,
              const Clock& now, size_t max_entries);
    void revoke(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account,
                const Clock& now, size_t max_entries);
    // Somebody voiced them: a loss of voice is forgotten, what was earned stays
    void restore(const std::string& network, const std::string& channel, const std::string& userhost, const std::string& account);
    void forget_older_than(const Clock& now, int days);
    size_t size();

    void load(const std::string& path, const Clock& now);
    // A loss of voice is written at once; the rest at most every `interval` seconds unless forced
    bool save(const std::string& path, const Clock& now, int interval, bool force);

private:
    struct Record {
        uint64_t key;
        uint32_t when; // wall clock, 0 = unknown
        bool revoked;
    };

    std::mutex mutex_;
    PsramVector<Record> records_; // least recently used first
    bool changed_ = false;
    bool urgent_ = false;
    int64_t saved_at_ = 0;

    Record* find_locked(uint64_t key);
    void put_locked(uint64_t key, bool revoked, const Clock& now, size_t max_entries);
};
