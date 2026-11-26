#include "record_file.h"
#include <cstdlib>

namespace record {

std::vector<std::string> split(const char* line) {
    std::vector<std::string> fields(1);
    for (const char* c = line; *c && *c != '\n'; c++) {
        if (*c == '\t') fields.emplace_back();
        else fields.back() += *c;
    }
    return fields;
}

long long number(const std::string& text) { return strtoll(text.c_str(), nullptr, 10); }

} // namespace record
