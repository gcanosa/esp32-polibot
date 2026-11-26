#pragma once

// Fixed-size record fields, and files of one tab-separated record per line. Reading and writing go
// line by line, so a long list never needs a large buffer in RAM.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

namespace record {

// Copies text into a record field: no tabs or line breaks (they separate fields on disk), and a
// cut-off never leaves half a UTF-8 character behind
template <size_t N>
void set(char (&field)[N], const std::string& value) {
    size_t length = std::min(value.size(), N - 1);
    if (length < value.size()) {
        while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80) length--;
    }
    for (size_t i = 0; i < length; i++) {
        const char c = value[i];
        field[i] = (c == '\t' || c == '\r' || c == '\n' || c == '\0') ? ' ' : c;
    }
    field[length] = '\0';
}

// Compares a stored field with a value that may be longer than the field
template <size_t N>
bool is(const char (&field)[N], const std::string& value) {
    return strncasecmp(field, value.c_str(), N - 1) == 0;
}

std::vector<std::string> split(const char* line);
long long number(const std::string& text);

// Calls `use` with the fields of every line. Falls back to the temporary file of an interrupted save.
template <typename F>
void read_lines(const std::string& path, F use) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) f = fopen((path + ".tmp").c_str(), "r");
    if (!f) return;
    char line[640];
    while (fgets(line, sizeof(line), f)) {
        if (strchr(line, '\n')) use(split(line)); // an unfinished last line is ignored
    }
    fclose(f);
}

// Never truncate the only good copy: `write` fills a sibling file, which is then swapped in
template <typename F>
bool write_file(const std::string& path, F write) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    bool ok = write(f);
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    remove(path.c_str()); // SPIFFS rename() refuses to overwrite
    return rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace record
