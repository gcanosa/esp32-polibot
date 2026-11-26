#pragma once

// Hostmasks, bans and channel names. No ESP-IDF, so it can be tested on a PC.
#include <string>

bool wildcard_match(const std::string& pattern, const std::string& text); // * and ?, ignoring case

// True if the mask names something particular, so that it cannot match everybody ("*!*@*")
bool mask_is_specific(const std::string& mask);

// True if something besides wildcards and dots follows the "@". Only such a mask identifies somebody
// without a password: nicknames and idents are the sender's to choose.
bool mask_names_host(const std::string& mask);

std::string user_host(const std::string& prefix); // "nick!user@host" -> "user@host", "" if there is none
bool is_channel_name(const std::string& name);

// What a ban or quiet aimed at `target` should match. A mask is used as typed; for a nick, its
// user@host (if known) decides, according to style "host", "userhost" or "nick".
std::string ban_mask(const std::string& target, const std::string& userhost, const std::string& style);

bool is_netsplit_reason(const std::string& reason); // "irc.a.net irc.b.net"
