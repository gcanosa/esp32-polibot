#include "auto_modes.h"
#include <algorithm>
#include <cstring>
#include "irc_masks.h"

uint8_t auto_bit(char mode) {
    return mode == 'o' || mode == 'a' || mode == 'q' ? AUTO_OP : mode == 'h' ? AUTO_HALFOP : mode == 'v' ? AUTO_VOICE : 0;
}

std::vector<std::string> grant_lines(const std::vector<std::pair<char, std::string>>& grants, size_t per_line) {
    std::vector<std::string> lines;
    per_line = std::max<size_t>(per_line, 1);
    for (size_t first = 0; first < grants.size(); first += per_line) {
        std::string letters = "+", names;
        for (size_t i = first; i < std::min(grants.size(), first + per_line); i++) {
            letters += grants[i].first;
            names += " " + grants[i].second;
        }
        lines.push_back(letters + names);
    }
    return lines;
}

bool mask_restricts(std::string mask, const std::string& hostmask, const std::string& account) {
    bool by_account = false;
    // what stands before the mask names the kind of ban; colons after "!" or "@" belong to an IPv6 host
    for (size_t colon; (colon = mask.find(':')) != std::string::npos && colon < mask.find_first_of("!@");) {
        const std::string kind = mask.substr(0, colon);
        by_account = kind == "$a" || kind == "~a" || kind == "~account" || kind == "R";
        mask.erase(0, colon + 1);
    }
    if (!mask_is_specific(mask)) return false;
    if (by_account) return !account.empty() && wildcard_match(mask, account);
    return mask.find_first_of("!@") != std::string::npos && wildcard_match(mask, hostmask);
}

uint32_t after_pause(uint32_t since, uint32_t paused_at, uint32_t now) {
    return since >= paused_at ? now : since + (now - paused_at);
}
