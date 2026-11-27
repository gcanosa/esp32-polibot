#include "releases.h"
#include <algorithm>
#include <array>
#include <cctype>

using Version = std::array<long, 4>;

static bool parse_version(const std::string& text, Version& out) {
    out = {};
    size_t part = 0, digits = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9' && digits < 6) {
            out[part] = out[part] * 10 + (c - '0');
            digits++;
        } else if (c == '.' && digits > 0 && part + 1 < out.size()) {
            part++;
            digits = 0;
        } else {
            return false;
        }
    }
    return digits > 0;
}

bool version_newer(const std::string& candidate, const std::string& current) {
    Version a, b;
    return parse_version(candidate, a) && parse_version(current, b) && a > b;
}

bool release_file_name(const std::string& name) {
    if (name.size() < 5 || name.size() > 64 || name.front() == '.' || name.compare(name.size() - 4, 4, ".bin") != 0) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == '_'; });
}

bool sha256_text(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
