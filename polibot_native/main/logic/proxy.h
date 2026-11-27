#pragma once

// A reverse proxy in front of the panel names the visitor in X-Forwarded-For. It is believed only from
// the addresses listed in the settings: anybody can write that header.
#include <string>

// Nothing, or up to 8 IPv4 addresses separated by commas or spaces
bool valid_proxy_list(const std::string& list);
bool proxy_listed(const std::string& list, const std::string& address);
// The address the proxy itself added, which is the last one in the header; "" if that is no address
std::string forwarded_address(const std::string& header);
