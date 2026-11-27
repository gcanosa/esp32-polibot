#pragma once

// What a line of spam can carry: addresses, other channels' names and nicknames named in bulk
#include <string>
#include <vector>

// The text without colour and style codes, which would otherwise hide an address
std::string strip_formatting(const std::string& text);

// The first advertisement in a line, in lower case: the host of a web address, or (if `channels`) the
// name of a channel other than `here`. "" if there is none. `allowed` names domains, separated by commas
// or spaces, whose addresses and subdomains are welcome.
std::string find_advert(const std::string& message, const std::string& here, bool channels, const std::string& allowed);

// The words of a line that could be nicknames, each once, in lower case
std::vector<std::string> name_words(const std::string& message);
