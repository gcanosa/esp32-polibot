#pragma once

// What the bot remembers besides its settings. Kept in external RAM and saved to the settings
// partition. No ESP-IDF here, so it can be tested on a PC (test/memory_test.cpp).
#include <mutex>
#include <string>
#include "ban_list.h"
#include "mailbox.h"
#include "seen_list.h"
#include "voice_memory.h"

class BotMemory {
public:
    SeenList seen;
    Mailbox mail;
    BanList bans;
    VoiceMemory voices; // timed voice: earned or taken away

    void load(const std::string& directory, const Clock& now);
    // Writes what changed; "seen" and earned voice only every seen_interval seconds unless forced.
    // False if a file could not be written.
    bool save(const std::string& directory, const Clock& now, int seen_interval, bool force);

private:
    std::mutex saving_; // one writer at a time
};

extern BotMemory g_memory;
