#pragma once

// Channel modes the bot keeps set or unset, written like "+nt-s". Only plain switches can be locked:
// no ranks, no lists and nothing that takes a value (the floating limit has its own rules).
#include <string>
#include <vector>
#include "irc_modes.h"

// The lock in its tidy form ("+nt-s"); "" if the text cannot be one
std::string clean_mode_lock(const std::string& text);

// The MODE argument that undoes what these changes did against the lock, such as "+t-s"; "" if nothing.
// Letters in `skip` are left alone.
std::string broken_modes(const std::string& lock, const std::vector<ModeChange>& changes, const std::string& skip);

// What has to change on a channel that has the modes `present` for the lock to hold
std::string missing_modes(const std::string& lock, const std::string& present, const std::string& skip);
