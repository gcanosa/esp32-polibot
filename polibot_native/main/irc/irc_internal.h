#pragma once

// Small helpers shared by the IRCClient source files
#include <esp_timer.h>
#include <strings.h>
#include <cstdint>
#include <string>

constexpr int SASL_TIMEOUT_SECS = 15; // how long a server may take to sign the bot in

inline int64_t seconds_now() { return esp_timer_get_time() / 1000000; }

// IRC names ignore case
inline bool same(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) == 0; }
