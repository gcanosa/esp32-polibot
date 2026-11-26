#include "irc_masks.h"
#include <cctype>
#include <sstream>

bool wildcard_match(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, match = 0;
    size_t star = std::string::npos;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (p < pattern.size() && (pattern[p] == '?' || std::tolower((unsigned char)pattern[p]) == std::tolower((unsigned char)text[t]))) {
            p++;
            t++;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

bool mask_is_specific(const std::string& mask) {
    return mask.find_first_not_of("*?!@.~") != std::string::npos;
}

bool mask_names_host(const std::string& mask) {
    const size_t at = mask.rfind('@');
    return at != std::string::npos && mask.find_first_not_of("*?.:~", at + 1) != std::string::npos;
}

std::string user_host(const std::string& prefix) {
    const size_t bang = prefix.find('!');
    return bang == std::string::npos ? "" : prefix.substr(bang + 1);
}

bool is_channel_name(const std::string& s) {
    return s.size() >= 2 && s.size() <= 64 && (s[0] == '#' || s[0] == '&') &&
           s.find_first_of(" ,\x07\"'<>`\\") == std::string::npos;
}

std::string ban_mask(const std::string& target, const std::string& userhost, const std::string& style) {
    if (target.empty()) return "";
    // extended bans and full masks are used as typed
    if (target[0] == '$' || target[0] == '~' || target.find('!') != std::string::npos) return target;
    if (target.find('@') != std::string::npos) return "*!" + target;
    const size_t at = userhost.rfind('@');
    if (at != std::string::npos && at + 1 < userhost.size()) {
        const std::string host = userhost.substr(at + 1);
        if (style == "userhost" && at > 0) return "*!" + userhost;
        if (style == "host" && mask_is_specific(host)) return "*!*@" + host;
    }
    return target + "!*@*";
}

bool is_netsplit_reason(const std::string& reason) {
    std::istringstream words(reason);
    std::string a, b, extra;
    if (!(words >> a >> b) || (words >> extra)) return false;
    auto server = [](const std::string& s) {
        const size_t dot = s.find('.');
        return dot != std::string::npos && dot > 0 && s.back() != '.' && s.find_first_of(":/@!?,") == std::string::npos;
    };
    return server(a) && server(b);
}
