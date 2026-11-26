#pragma once

#include <string>
#include <cJSON.h>

// Why the device last started, in words
const char* restart_reason();

// Starts measuring the CPU load
void system_info_start();

// Hardware and usage figures for the public home page (caller owns the result).
// Nothing in here identifies the network, the servers, the channels or the users.
cJSON* system_info_json();

// The same in one line, for the IRC "sysinfo" command
std::string system_info_line();
