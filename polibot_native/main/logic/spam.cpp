#include "spam.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include "irc_text.h"

namespace {

bool alnum(char c) { return std::isalnum((unsigned char)c); }

// Calls `each` with every stretch of the line between separators; stops when it returns true
template <typename F>
void words(const std::string& line, const char* separators, F each) {
    for (size_t at = line.find_first_not_of(separators); at != std::string::npos;) {
        const size_t end = std::min(line.find_first_of(separators, at), line.size());
        if (each(line.substr(at, end - at))) return;
        at = line.find_first_not_of(separators, end);
    }
}

// The host of the web address in a lower case word, "" if it is none. Without "http://" or "www." only
// "domain.tld/something" counts, so that "and/or" or "3.5/10" are left alone.
std::string web_host(const std::string& word) {
    size_t start = 0;
    bool certain = false;
    const size_t scheme = word.find("://");
    if (scheme != std::string::npos) {
        for (const char* known : {"http", "https", "ftp", "irc", "ircs"}) {
            const size_t n = std::strlen(known);
            certain = certain || (scheme >= n && word.compare(scheme - n, n, known) == 0);
        }
        if (!certain) return "";
        start = scheme + 3;
    } else {
        while (start < word.size() && !alnum(word[start])) start++;
        certain = word.compare(start, 4, "www.") == 0;
    }
    const size_t path = std::min(word.find_first_of("/?#", start), word.size());
    std::string host = word.substr(start, path - start);
    const size_t at = host.rfind('@');
    if (at != std::string::npos) {
        if (scheme == std::string::npos) return ""; // an e-mail address
        host.erase(0, at + 1);
    }
    host = host.substr(0, host.find(':'));
    while (!host.empty() && !alnum(host.back())) host.pop_back();
    if (certain) return host;
    const size_t dot = host.rfind('.');
    if (path >= word.size() || word[path] != '/' || dot == std::string::npos || dot == 0 || host.size() - dot < 3) return "";
    for (size_t i = 0; i < host.size(); i++) {
        if (i > dot ? !std::isalpha((unsigned char)host[i]) : !(alnum(host[i]) || host[i] == '-' || host[i] == '.')) return "";
    }
    return host;
}

bool welcome(const std::string& host, const std::string& allowed) {
    bool found = false;
    words(irc_lower(allowed), ", ", [&](const std::string& domain) {
        found = host == domain || (host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
                                   host[host.size() - domain.size() - 1] == '.');
        return found;
    });
    return found;
}

std::string trimmed(const std::string& word, const char* front, const char* back) {
    const size_t start = word.find_first_not_of(front);
    if (start == std::string::npos) return "";
    const size_t end = word.find_last_not_of(back);
    return end == std::string::npos || end < start ? "" : word.substr(start, end - start + 1);
}

} // namespace

std::string strip_formatting(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = text[i];
        if (c == 0x03) { // a colour: up to two digits, and two more after a comma
            auto digits = [&] {
                for (int n = 0; n < 2 && i + 1 < text.size() && std::isdigit((unsigned char)text[i + 1]); n++) i++;
            };
            digits();
            if (i + 2 < text.size() && text[i + 1] == ',' && std::isdigit((unsigned char)text[i + 2])) {
                i++;
                digits();
            }
        } else if (c != 0x02 && c != 0x0f && c != 0x11 && c != 0x16 && c != 0x1d && c != 0x1e && c != 0x1f) {
            out += (char)c;
        }
    }
    return out;
}

std::string find_advert(const std::string& message, const std::string& here, bool channels, const std::string& allowed) {
    const std::string room = irc_lower(here);
    std::string found;
    words(irc_lower(strip_formatting(message)), " \t\x01", [&](const std::string& word) {
        const std::string host = web_host(word);
        if (!host.empty() && !welcome(host, allowed)) found = host;
        if (found.empty() && channels) {
            const std::string name = trimmed(word, "([<{\"'", ".,;:!?)]>}\"'");
            const size_t letters = name.find_first_not_of('#');
            // "#1" is a number, not a channel
            if (name.size() >= 3 && name[0] == '#' && letters != std::string::npos && std::isalpha((unsigned char)name[letters]) && name != room) {
                found = name;
            }
        }
        return !found.empty();
    });
    return found;
}

std::vector<std::string> name_words(const std::string& message) {
    std::vector<std::string> out;
    words(irc_lower(strip_formatting(message)), " ,\t\x01", [&](const std::string& word) {
        const std::string name = trimmed(word, "@(<\"'", ":;.!?)>\"'");
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        return false;
    });
    return out;
}
