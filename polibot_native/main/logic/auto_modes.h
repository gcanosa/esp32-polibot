#pragma once

// Operator, half-operator and voice given by the bot itself. No ESP-IDF, so it can be tested on a PC.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

constexpr uint8_t AUTO_OP = 1, AUTO_HALFOP = 2, AUTO_VOICE = 4;

uint8_t auto_bit(char mode); // the bit of 'o', 'h' or 'v'; owner and admin count as 'o'; 0 for any other

// "+ovv amy bo cy": the mode lines for these (letter, nickname) pairs, at most per_line in each
std::vector<std::string> grant_lines(const std::vector<std::pair<char, std::string>>& grants, size_t per_line);

// True if a ban or mute on `mask` ("*!*@host", "m:*!*@host", "~q:nick!*@*", "$a:account") means this user
bool mask_restricts(std::string mask, const std::string& hostmask, const std::string& account);

// The new start of a member's waiting time after the clock stood still from paused_at until now
uint32_t after_pause(uint32_t since, uint32_t paused_at, uint32_t now);
