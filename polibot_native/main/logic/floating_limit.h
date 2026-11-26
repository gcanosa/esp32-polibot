#pragma once
// The floating +l of one channel: when to change it and to what, from how many people are there.
// It is looked at on an interval, never on each join, so a flood runs into the limit instead of
// pushing it up. No ESP-IDF here (see test/logic_test.cpp).
#include <algorithm>
#include <cstdint>

struct LimitRules {
    int margin;      // free places above the people present
    int floor;       // never set below this
    int slack;       // extra places on a raise; lowered only when more than this too high
    int lower_after; // seconds it must have been too high before it comes down
};

struct FloatingLimit {
    int current = 0;         // the +l the bot set, 0 = none
    int64_t high_since = -1; // since when it has been too high

    // The limit to set now, or 0 to leave the channel as it is
    int next(int users, const LimitRules& rules, int64_t now) {
        if (users <= 0) return 0; // nobody counted: a guess could lock everyone out
        const int excess = current - std::max(rules.floor, users + rules.margin);
        if (!current || excess < 0) {
            high_since = -1;
            return current = std::max(rules.floor, users + rules.margin + rules.slack);
        }
        if (excess <= rules.slack) {
            high_since = -1;
            return 0;
        }
        if (high_since < 0) high_since = now;
        if (now - high_since < rules.lower_after) return 0;
        // in halves, and always by more than the slack, so that no MODE moves it by a trifle
        return current -= std::min(excess, std::max((excess + 1) / 2, rules.slack + 1));
    }
};
