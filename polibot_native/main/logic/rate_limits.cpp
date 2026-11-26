#include "rate_limits.h"
#include <algorithm>
#include <cctype>
#include <cstring>

uint32_t key_hash(const std::string& key) { // FNV-1a, ignoring case
    uint32_t hash = 2166136261u;
    for (unsigned char c : key) hash = (hash ^ (uint32_t)std::tolower(c)) * 16777619u;
    return hash ? hash : 1;
}

bool TokenBucket::take(int64_t now_ms, int burst, int interval_ms) {
    if (interval_ms <= 0) return true;
    const int64_t capacity = (int64_t)std::max(burst, 1) * interval_ms;
    if (last_ms_ < 0) credit_ms_ = capacity; // starts full
    else credit_ms_ = std::min(capacity, credit_ms_ + (now_ms - last_ms_));
    last_ms_ = now_ms;
    if (credit_ms_ < interval_ms) return false;
    credit_ms_ -= interval_ms;
    return true;
}

int RateWindow::hit(const std::string& key, int64_t now, int window) {
    const uint32_t hash = key_hash(key);
    const Event event = {hash, (uint32_t)now};
    if (events_.size() < capacity_) {
        if (events_.empty()) events_.reserve(capacity_);
        events_.push_back(event);
    } else {
        events_[next_] = event;
        next_ = (next_ + 1) % capacity_;
    }
    int count = 0;
    for (const auto& e : events_) {
        if (e.key == hash && (int64_t)now - e.when < window) count++;
    }
    return std::max(count, 1); // this one counts even with an empty window
}

void RateWindow::clear(const std::string& key) {
    const uint32_t hash = key_hash(key);
    for (auto& e : events_) {
        if (e.key == hash) e.key = 0;
    }
}

void ExpiringSet::add(const std::string& key, int64_t now, int64_t until) {
    const Entry entry = {key_hash(key), (uint32_t)until};
    Entry* victim = nullptr;
    for (auto& e : entries_) {
        if (e.key == entry.key || (int64_t)e.until <= now) {
            e = entry;
            return;
        }
        if (!victim || e.until < victim->until) victim = &e;
    }
    if (entries_.size() < capacity_) entries_.push_back(entry);
    else if (victim) *victim = entry; // full: the one ending soonest makes room
}

int64_t ExpiringSet::remaining(const std::string& key, int64_t now) const {
    const uint32_t hash = key_hash(key);
    for (const auto& e : entries_) {
        if (e.key == hash && (int64_t)e.until > now) return (int64_t)e.until - now;
    }
    return 0;
}

void ExpiringSet::clear() {
    entries_.clear();
    entries_.shrink_to_fit();
}

bool LoginGuard::failed(const std::string& source, int64_t now, int max_attempts, int window, int block) {
    if (failures_.hit(source, now, window) < std::min(max_attempts, MAX_ATTEMPTS)) return false;
    blocked_.add(source, now, now + block);
    failures_.clear(source);
    return true;
}

// Lower case, single spaces, without colour and formatting codes: small changes do not make a new text
static std::string normalise(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = text[i];
        if (c == 3) { // colour: followed by up to two digits, and optionally a comma and two more
            auto digits = [&] { for (int n = 0; n < 2 && i + 1 < text.size() && std::isdigit((unsigned char)text[i + 1]); n++) i++; };
            digits();
            if (i + 2 < text.size() && text[i + 1] == ',' && std::isdigit((unsigned char)text[i + 2])) {
                i++;
                digits();
            }
        } else if (c == ' ') {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else if (c >= 32) {
            out += (char)std::tolower(c);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<std::string> CloneDetector::add(const std::string& channel, const std::string& userhost, const std::string& nick,
                                            const std::string& text, int64_t now, int users, int window) {
    Line line = {key_hash(channel + '\n' + normalise(text)), key_hash(userhost), (uint32_t)now, false, {}};
    strncpy(line.nick, nick.c_str(), sizeof(line.nick) - 1);
    if (lines_.size() < LINES) {
        if (lines_.empty()) lines_.reserve(LINES);
        lines_.push_back(line);
    } else {
        lines_[next_] = line;
        next_ = (next_ + 1) % LINES;
    }

    auto matches = [&](const Line& other) { return other.text == line.text && (int64_t)now - other.when < window; };
    uint32_t seen[LINES];
    int different = 0;
    for (const auto& other : lines_) {
        if (matches(other) && std::find(seen, seen + different, other.user) == seen + different) seen[different++] = other.user;
    }
    std::vector<std::string> nicks;
    if (different < users) return nicks;
    for (auto& other : lines_) {
        if (!matches(other) || other.returned) continue;
        other.returned = true;
        if (std::find(nicks.begin(), nicks.end(), other.nick) == nicks.end()) nicks.push_back(other.nick);
    }
    return nicks;
}

int Strikes::add(const std::string& key, int64_t now, int reset) {
    const uint32_t hash = key_hash(key);
    Entry* slot = nullptr;
    for (auto& e : entries_) {
        if (e.who == hash) {
            e.count = (int64_t)now - e.last < reset ? e.count + 1 : 1;
            e.last = (uint32_t)now;
            return e.count;
        }
        if (!slot || e.last < slot->last) slot = &e;
    }
    const Entry entry = {hash, (uint32_t)now, 1};
    if (entries_.size() < ENTRIES) entries_.push_back(entry);
    else *slot = entry; // full: the oldest offence is forgotten
    return 1;
}
