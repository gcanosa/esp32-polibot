#include "members.h"
#include <cstring>
#include <strings.h>

template <size_t N>
static void copy_field(char (&to)[N], const std::string& from) {
    strncpy(to, from.c_str(), N - 1);
    to[N - 1] = '\0';
}

bool Member::has_mode(char mode) const { return mode && strchr(modes, mode); }

bool Member::is_operator() const { return strpbrk(modes, "qaoh") != nullptr; }

void Member::set_mode(char mode, bool on) {
    if (!mode || on == has_mode(mode)) return;
    const size_t length = strlen(modes);
    if (on) {
        if (length + 1 < sizeof(modes)) {
            modes[length] = mode;
            modes[length + 1] = '\0';
        }
    } else {
        char* at = strchr(modes, mode);
        memmove(at, at + 1, strlen(at));
    }
}

void Member::set_userhost(const std::string& value) {
    if (!value.empty()) copy_field(userhost, value);
}

// Longer nicknames than the field holds are compared by what fits
static bool same_nick(const char* stored, const std::string& nick) {
    return strncasecmp(stored, nick.c_str(), sizeof(Member::nick) - 1) == 0;
}

Member* MemberList::find(const std::string& nick) {
    for (auto& m : members_) {
        if (same_nick(m.nick, nick)) return &m;
    }
    return nullptr;
}

Member& MemberList::add(const std::string& nick) {
    if (Member* m = find(nick)) return *m;
    Member m = {};
    copy_field(m.nick, nick);
    members_.push_back(m);
    return members_.back();
}

bool MemberList::remove(const std::string& nick) {
    Member* m = find(nick);
    if (!m) return false;
    *m = members_.back();
    members_.pop_back();
    return true;
}

bool MemberList::rename(const std::string& from, const std::string& to) {
    Member* m = find(from);
    if (!m) return false;
    copy_field(m->nick, to);
    return true;
}
