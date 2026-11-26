#pragma once

// Text of the IRC "help" command. No ESP-IDF here, so it can be tested on a PC
// (see test/command_help_test.cpp).
#include <string>
#include <vector>

// Lines answering "help" (empty topic) or "help <command>", written with the configured prefix.
// role_level: 10 user, 50 moderator, 75 admin, 100 owner (see get_role_level);
// 0 for an answer said aloud in a channel: the general commands only, and nobody's role.
std::vector<std::string> help_text(const std::string& topic, const std::string& prefix, const std::string& bot_nick,
                                   int role_level, const std::string& version);
