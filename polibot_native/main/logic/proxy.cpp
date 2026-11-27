#include "proxy.h"
#include <algorithm>
#include <cctype>

namespace {

bool ipv4(const std::string& text) {
    int parts = 0, digits = 0, value = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i < text.size() && std::isdigit((unsigned char)text[i])) {
            value = value * 10 + (text[i] - '0');
            if (++digits > 3 || value > 255) return false;
        } else if (digits && (i == text.size() || text[i] == '.')) {
            parts++;
            digits = value = 0;
        } else {
            return false;
        }
    }
    return parts == 4;
}

// Calls `each` with every entry of the list until it returns false
template <typename F>
bool every_entry(const std::string& list, F each) {
    for (size_t at = list.find_first_not_of(", "); at != std::string::npos;) {
        const size_t end = std::min(list.find_first_of(", ", at), list.size());
        if (!each(list.substr(at, end - at))) return false;
        at = list.find_first_not_of(", ", end);
    }
    return true;
}

} // namespace

bool valid_proxy_list(const std::string& list) {
    int count = 0;
    return every_entry(list, [&](const std::string& entry) { return ++count <= 8 && ipv4(entry); });
}

bool proxy_listed(const std::string& list, const std::string& address) {
    return !every_entry(list, [&](const std::string& entry) { return entry != address; });
}

std::string forwarded_address(const std::string& header) {
    const size_t comma = header.rfind(',');
    const size_t start = header.find_first_not_of(" \t", comma == std::string::npos ? 0 : comma + 1);
    if (start == std::string::npos) return "";
    const std::string last = header.substr(start, header.find_last_not_of(" \t") - start + 1);
    if (ipv4(last)) return last;
    // IPv6, possibly ending in an IPv4 address; it only has to be harmless to show and to count by
    const bool ipv6 = last.size() <= 45 && std::count(last.begin(), last.end(), ':') >= 2 &&
                      std::all_of(last.begin(), last.end(), [](unsigned char c) { return std::isxdigit(c) || c == ':' || c == '.'; });
    return ipv6 ? last : "";
}
