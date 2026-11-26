#pragma once

// Each returns an empty string if the section is acceptable, otherwise the reason it is not.
// The "irc" section is checked by validate_irc_config() in irc_config.h.
#include <string>
#include <cJSON.h>

std::string validate_wifi_config(const cJSON* wifi);
std::string validate_guardian_config(const cJSON* guardian);
