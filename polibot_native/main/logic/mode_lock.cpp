#include "mode_lock.h"
#include <cctype>
#include <cstring>

namespace {

// ranks, lists and modes with a value on one server or another
constexpr const char* NOT_LOCKABLE = "abefhjkloqvILX";

// Walks a tidy lock: `each(letter, wanted_on)`
template <typename F>
void each_locked(const std::string& lock, F each) {
    bool on = true;
    for (char c : lock) {
        if (c == '+' || c == '-') on = c == '+';
        else each(c, on);
    }
}

std::string join(const std::string& on, const std::string& off) {
    return (on.empty() ? "" : "+" + on) + (off.empty() ? "" : "-" + off);
}

} // namespace

std::string clean_mode_lock(const std::string& text) {
    std::string on, off;
    bool adding = true;
    for (char c : text) {
        if (c == '+' || c == '-') {
            adding = c == '+';
            continue;
        }
        if (!std::isalpha((unsigned char)c) || std::strchr(NOT_LOCKABLE, c)) return "";
        // the last word on a letter counts
        for (std::string* list : {&on, &off}) {
            const size_t at = list->find(c);
            if (at != std::string::npos) list->erase(at, 1);
        }
        (adding ? on : off) += c;
    }
    return on.size() + off.size() > 16 ? "" : join(on, off);
}

std::string broken_modes(const std::string& lock, const std::vector<ModeChange>& changes, const std::string& skip) {
    std::string on, off;
    each_locked(lock, [&](char letter, bool wanted) {
        if (skip.find(letter) != std::string::npos) return;
        bool wrong = false;
        for (const auto& c : changes) {
            if (c.mode == letter) wrong = c.add != wanted; // the last change in the line counts
        }
        if (wrong) (wanted ? on : off) += letter;
    });
    return join(on, off);
}

std::string missing_modes(const std::string& lock, const std::string& present, const std::string& skip) {
    std::string on, off;
    each_locked(lock, [&](char letter, bool wanted) {
        if (skip.find(letter) == std::string::npos && (present.find(letter) != std::string::npos) != wanted) (wanted ? on : off) += letter;
    });
    return join(on, off);
}
