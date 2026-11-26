#pragma once

// Who gets operator, half-operator or voice in a channel without asking. Every entry belongs to one
// channel of one network. Fixed-size records in external RAM. No locking: the owner does that.
#include <cstdint>
#include <string>
#include <vector>
#include "psram_alloc.h"

struct AccessEntry {
    char network[33];
    char channel[65];
    char who[96]; // a "nick!user@host" mask, "user:<account of the bot>", or a services account
    char mode;    // 'o', 'h' or 'v'
};

// "user@host" as a full mask; anything else as typed
std::string access_mask(const std::string& typed);

// Empty if `who` may stand in a list, otherwise why not. A nickname alone identifies nobody, so a bare
// word is always a services account, and a mask has to name a host.
std::string access_problem(const std::string& who);

inline bool is_list_mode(char mode) { return mode == 'o' || mode == 'h' || mode == 'v'; }

class AccessList {
public:
    static constexpr size_t MAX = 200;

    // Adding what is there already changes nothing and succeeds; false if full or a value does not fit
    bool add(const std::string& network, const std::string& channel, char mode, const std::string& who);
    bool remove(const std::string& network, const std::string& channel, char mode, const std::string& who);
    bool any(const std::string& network, const std::string& channel) const;
    // The AUTO_* bits of the entries that mean this person; account and user are "" when unknown
    uint8_t modes_for(const std::string& network, const std::string& channel, const std::string& hostmask,
                      const std::string& account, const std::string& user) const;
    // The entries of one channel with this mode
    std::vector<std::string> list(const std::string& network, const std::string& channel, char mode) const;
    const PsramVector<AccessEntry>& all() const { return entries_; }
    void clear() { entries_.clear(); }

private:
    PsramVector<AccessEntry> entries_;
};
