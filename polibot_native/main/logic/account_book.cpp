#include "account_book.h"
#include <cstring>
#include <strings.h>

template <size_t N>
static void copy_field(char (&to)[N], const std::string& from) {
    strncpy(to, from.c_str(), N - 1);
    to[N - 1] = '\0';
}

// Values longer than the field are compared by what fits
template <size_t N>
static bool is(const char (&field)[N], const std::string& value) {
    return strncasecmp(field, value.c_str(), N - 1) == 0;
}

int AccountBook::index_of(const std::string& network, const std::string& nick) const {
    for (size_t i = 0; i < entries_.size(); i++) {
        if (is(entries_[i].nick, nick) && is(entries_[i].network, network)) return (int)i;
    }
    return -1;
}

void AccountBook::erase(size_t index) {
    entries_[index] = entries_.back();
    entries_.pop_back();
}

bool AccountBook::note(const std::string& network, const std::string& nick, const std::string& userhost, const std::string& account) {
    const int at = index_of(network, nick);
    // an account name too long to store must not be matched by its beginning
    if (account.empty() || account.size() >= sizeof(Entry::account)) {
        if (at >= 0) erase(at);
        return false;
    }
    if (at >= 0 && is(entries_[at].account, account) && is(entries_[at].userhost, userhost)) return false;
    Entry entry = {};
    copy_field(entry.network, network);
    copy_field(entry.nick, nick);
    copy_field(entry.userhost, userhost);
    copy_field(entry.account, account);
    if (at >= 0) {
        entries_[at] = entry;
    } else if (entries_.size() < CAPACITY) {
        entries_.push_back(entry);
    } else {
        entries_[next_] = entry;
        next_ = (next_ + 1) % CAPACITY;
    }
    return true;
}

std::string AccountBook::find(const std::string& network, const std::string& nick, const std::string& userhost) const {
    const int at = index_of(network, nick);
    return at >= 0 && !userhost.empty() && is(entries_[at].userhost, userhost) ? entries_[at].account : "";
}

void AccountBook::rename(const std::string& network, const std::string& from, const std::string& to) {
    // whoever held the new nickname before is gone
    const int old_holder = index_of(network, to);
    if (old_holder >= 0 && strcasecmp(from.c_str(), to.c_str()) != 0) erase(old_holder);
    const int at = index_of(network, from);
    if (at >= 0) copy_field(entries_[at].nick, to);
}

void AccountBook::forget(const std::string& network, const std::string& nick) {
    const int at = index_of(network, nick);
    if (at >= 0) erase(at);
}

void AccountBook::forget_network(const std::string& network) {
    for (size_t i = entries_.size(); i-- > 0;) {
        if (is(entries_[i].network, network)) erase(i);
    }
}
