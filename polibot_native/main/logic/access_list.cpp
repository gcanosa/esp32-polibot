#include "access_list.h"
#include <algorithm>
#include <cstring>
#include <strings.h>
#include "auto_modes.h"
#include "irc_masks.h"

static bool is(const char* stored, const std::string& value) { return strcasecmp(stored, value.c_str()) == 0; }

static bool in_channel(const AccessEntry& e, const std::string& network, const std::string& channel) {
    return is(e.channel, channel) && is(e.network, network);
}

std::string access_mask(const std::string& typed) {
    return typed.find('@') != std::string::npos && typed.find('!') == std::string::npos ? "*!" + typed : typed;
}

std::string access_problem(const std::string& who) {
    if (who.empty() || who.size() >= sizeof(AccessEntry::who) ||
        std::any_of(who.begin(), who.end(), [](unsigned char c) { return c <= ' ' || c == ','; })) {
        return "An entry has 1-95 characters, without spaces or commas.";
    }
    if (who.find_first_of("!@") != std::string::npos) {
        if (who.find('!') == std::string::npos || !mask_names_host(who)) {
            return "A hostmask must name a host, e.g. *!*@host.example.";
        }
        return "";
    }
    const bool bot_user = who.rfind("user:", 0) == 0;
    const std::string name = bot_user ? who.substr(5) : who;
    // 31: the longest services account the bot keeps; 32: the longest account of its own
    if (name.empty() || name.size() > (bot_user ? 32u : 31u) || name.find_first_of("*?:") != std::string::npos) {
        return "Give a hostmask, a services account, or user:<account of the bot>.";
    }
    return "";
}

bool AccessList::add(const std::string& network, const std::string& channel, char mode, const std::string& who) {
    AccessEntry e = {};
    if (network.empty() || channel.empty() || network.size() >= sizeof(e.network) || channel.size() >= sizeof(e.channel) ||
        !is_list_mode(mode) || !access_problem(who).empty()) {
        return false;
    }
    for (const auto& old : entries_) {
        if (old.mode == mode && is(old.who, who) && in_channel(old, network, channel)) return true;
    }
    if (entries_.size() >= MAX) return false;
    memcpy(e.network, network.c_str(), network.size());
    memcpy(e.channel, channel.c_str(), channel.size());
    memcpy(e.who, who.c_str(), who.size());
    e.mode = mode;
    entries_.push_back(e);
    return true;
}

bool AccessList::remove(const std::string& network, const std::string& channel, char mode, const std::string& who) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const AccessEntry& e) {
        return e.mode == mode && is(e.who, who) && in_channel(e, network, channel);
    });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

bool AccessList::any(const std::string& network, const std::string& channel) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const AccessEntry& e) { return in_channel(e, network, channel); });
}

uint8_t AccessList::modes_for(const std::string& network, const std::string& channel, const std::string& hostmask,
                              const std::string& account, const std::string& user) const {
    uint8_t bits = 0;
    for (const auto& e : entries_) {
        if (!in_channel(e, network, channel)) continue;
        bool means;
        if (strpbrk(e.who, "!@")) means = wildcard_match(e.who, hostmask);
        else if (strncmp(e.who, "user:", 5) == 0) means = !user.empty() && is(e.who + 5, user);
        else means = !account.empty() && is(e.who, account);
        if (means) bits |= auto_bit(e.mode);
    }
    return bits;
}

std::vector<std::string> AccessList::list(const std::string& network, const std::string& channel, char mode) const {
    std::vector<std::string> names;
    for (const auto& e : entries_) {
        if (e.mode == mode && in_channel(e, network, channel)) names.push_back(e.who);
    }
    return names;
}
