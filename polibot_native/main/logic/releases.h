#pragma once
#include <string>

// "1.2.3": up to four numbers. True only if both read as such and `candidate` is the later one.
bool version_newer(const std::string& candidate, const std::string& current);

// A release file: letters, digits, dot, dash and underscore, ending in .bin, so it cannot leave its folder
bool release_file_name(const std::string& name);

// 64 lower case hexadecimal digits
bool sha256_text(const std::string& text);
