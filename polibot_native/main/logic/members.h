#pragma once

// Who is in a channel. Fixed-size records in one block per channel, so a big channel costs little.
#include <cstdint>
#include <string>
#include "psram_alloc.h"

struct Member {
    char nick[32];
    char userhost[96];
    char modes[7];   // member modes held, e.g. "ov"
    uint8_t settled; // automatic modes already decided for this stay (AUTO_* bits)
    uint32_t since;  // seconds since boot when first seen here, for timed voice; 0 = not noted yet

    bool is_operator() const; // owner, admin, operator or half-operator
    bool has_mode(char mode) const;
    void set_mode(char mode, bool on);
    void set_userhost(const std::string& value); // an empty value keeps what is known
};

class MemberList {
public:
    Member* find(const std::string& nick);
    Member& add(const std::string& nick); // the existing entry if there is one
    bool remove(const std::string& nick);
    bool rename(const std::string& from, const std::string& to);
    size_t size() const { return members_.size(); }
    const PsramVector<Member>& all() const { return members_; }
    PsramVector<Member>& all() { return members_; }

private:
    PsramVector<Member> members_;
};
