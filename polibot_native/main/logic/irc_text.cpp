#include "irc_text.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <strings.h>

std::string irc_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

bool contains_ci(const std::vector<std::string>& list, const std::string& item) {
    for (const auto& entry : list) {
        if (strcasecmp(entry.c_str(), item.c_str()) == 0) return true;
    }
    return false;
}

std::string join_words(const std::vector<std::string>& words, size_t from) {
    std::string out;
    for (size_t i = from; i < words.size(); i++) {
        if (i > from) out += " ";
        out += words[i];
    }
    return out;
}

std::string list_line(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); i++) {
        if (out.size() + items[i].size() > 350) return out + " (+" + std::to_string(items.size() - i) + " more)";
        if (i) out += ", ";
        out += items[i];
    }
    return out.empty() ? "(none)" : out;
}

bool parse_number(const std::string& s, int min, int max, int& out) {
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (s.empty() || *end || v < min || v > max) return false;
    out = v;
    return true;
}

bool parse_duration(const std::string& text, int& seconds) {
    if (text.size() < 2 || text.size() > 9) return false;
    const std::string digits = text.substr(0, text.size() - 1);
    if (digits.find_first_not_of("0123456789") != std::string::npos) return false;
    const char unit = std::tolower((unsigned char)text.back());
    const int64_t factor = unit == 's' ? 1 : unit == 'm' ? 60 : unit == 'h' ? 3600 : unit == 'd' ? 86400 : 0;
    int64_t total = 0;
    for (char c : digits) total = total * 10 + (c - '0');
    total *= factor;
    if (total <= 0 || total > 365 * 86400) return false;
    seconds = (int)total;
    return true;
}

std::string format_duration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const int64_t values[] = {seconds / 86400, seconds % 86400 / 3600, seconds % 3600 / 60, seconds % 60};
    const char units[] = {'d', 'h', 'm', 's'};
    for (int i = 0; i < 4; i++) {
        if (!values[i] && i < 3) continue;
        std::string out = std::to_string(values[i]) + units[i];
        if (i < 3 && values[i + 1]) out += " " + std::to_string(values[i + 1]) + units[i + 1];
        return out;
    }
    return "0s";
}

static void replace_all(std::string& text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
}

std::string fill_template(std::string text, const std::string& channel, const std::string& mask) {
    replace_all(text, "{channel}", channel);
    replace_all(text, "{mask}", mask);
    return text;
}

std::string client_command(const std::string& typed, const std::string& nick) {
    std::string line = trim(typed);
    if (!line.empty() && line[0] == '/') line = trim(line.substr(1));
    replace_all(line, "{nick}", nick);
    const size_t space = line.find(' ');
    const std::string word = irc_lower(line.substr(0, space));
    const std::string rest = space == std::string::npos ? "" : trim(line.substr(space + 1));
    if (word == "quote" || word == "raw") return rest;
    // a mode without a target means the bot's own
    if (word == "umode") return rest.empty() ? "" : "MODE " + nick + " " + rest;
    if (word == "mode" && !rest.empty() && (rest[0] == '+' || rest[0] == '-')) return "MODE " + nick + " " + rest;
    if (word == "msg" || word == "notice") {
        const size_t gap = rest.find(' ');
        if (gap == std::string::npos) return "";
        std::string text = trim(rest.substr(gap + 1));
        if (!text.empty() && text[0] == ':') text.erase(0, 1);
        return std::string(word == "msg" ? "PRIVMSG " : "NOTICE ") + rest.substr(0, gap) + " :" + text;
    }
    return line; // "join #chan", "oper name password": the server takes it as it is
}

std::string tag_value(const std::string& tags, const std::string& name) {
    for (size_t at = 0; at < tags.size();) {
        size_t end = tags.find(';', at);
        if (end == std::string::npos) end = tags.size();
        if (tags.compare(at, name.size(), name) == 0 && tags.size() > at + name.size() && tags[at + name.size()] == '=') {
            return tags.substr(at + name.size() + 1, end - at - name.size() - 1); // account names need no unescaping
        }
        at = end + 1;
    }
    return "";
}

std::string redact_command(const std::string& line) {
    std::istringstream iss(line);
    std::string word, normal;
    std::vector<std::string> words;
    while (iss >> word) {
        normal += (normal.empty() ? "" : " ") + word;
        words.push_back(word);
    }
    // a channel password follows the channel in "!join #chan password" and in a raw JOIN
    for (size_t i = 0; i + 2 < words.size(); i++) {
        const std::string command = irc_lower(words[i]);
        if (command != "!join" && !(command == "join" && (words[i + 1][0] == '#' || words[i + 1][0] == '&'))) continue;
        std::string kept;
        for (size_t k = 0; k < i + 2; k++) kept += (k ? " " : "") + words[k];
        return kept + " <hidden>";
    }
    const std::string low = " " + irc_lower(normal) + " ";
    // typed in the console, or sent raw to a server or service
    for (const char* marker : {"!login ", "!auth ", "!admin add ", "!admin passwd ", "identify ", "register ", " oper ", " pass ",
                               "password ", "+k ", "-k ", "+sk "}) {
        size_t at = low.find(marker);
        if (at == std::string::npos) continue;
        return trim(normal.substr(0, std::min(normal.size(), at + strlen(marker) - 1))) + " <hidden>";
    }
    return line;
}

std::vector<std::string> parse_steps(const std::string& list) {
    std::vector<std::string> steps;
    std::string word;
    for (size_t i = 0; i <= list.size(); i++) {
        if (i < list.size() && list[i] != ',' && list[i] != ' ') {
            word += (char)std::tolower((unsigned char)list[i]);
            continue;
        }
        if (word == "warn" || word == "quiet" || word == "kick" || word == "ban") steps.push_back(word);
        word.clear();
    }
    return steps;
}
