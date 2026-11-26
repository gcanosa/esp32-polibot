#pragma once

// Which services account each IRC user is signed in to, as the server reports it. A fixed number
// of entries in external RAM; when it is full the oldest ones are reused.
#include <string>
#include "psram_alloc.h"

class AccountBook {
public:
    static constexpr size_t CAPACITY = 512;

    // An empty account forgets the user. True if this tells something new about who they are.
    bool note(const std::string& network, const std::string& nick, const std::string& userhost, const std::string& account);
    // The account of this nickname, provided it still has this user@host; "" otherwise
    std::string find(const std::string& network, const std::string& nick, const std::string& userhost) const;
    void rename(const std::string& network, const std::string& from, const std::string& to);
    void forget(const std::string& network, const std::string& nick);
    void forget_network(const std::string& network);
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        char network[33];
        char nick[32];
        char userhost[96];
        char account[32];
    };
    PsramVector<Entry> entries_;
    size_t next_ = 0; // the entry reused when full

    int index_of(const std::string& network, const std::string& nick) const; // -1 if unknown
    void erase(size_t index);
};
