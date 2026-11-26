#pragma once

// Small string and time helpers. No ESP-IDF, so they can be tested on a PC.
#include <cstdint>
#include <string>
#include <vector>

std::string irc_lower(std::string s);
std::string trim(const std::string& s);
bool contains_ci(const std::vector<std::string>& list, const std::string& item); // ignoring case
std::string join_words(const std::vector<std::string>& words, size_t from = 0);
std::string list_line(const std::vector<std::string>& items); // comma separated, fits in one IRC message

// Whole-string number within [min, max]; std::stoi would abort the device on bad input
bool parse_number(const std::string& s, int min, int max, int& out);

// "30s", "10m", "2h", "7d" (a year at most). A bare number is not a duration: it may start a reason.
bool parse_duration(const std::string& text, int& seconds);
std::string format_duration(int64_t seconds); // the two largest units: "2d 3h", "5m 10s"

// "PRIVMSG ChanServ :OP {channel}" and "+q {mask}" with the placeholders filled in
std::string fill_template(std::string text, const std::string& channel, const std::string& mask = "");

// A line typed as in an IRC client ("/mode -x", "/msg NickServ ...") or as the server takes it, turned
// into what is sent. "{nick}" is the bot's nickname; "" for a line with nothing to send.
std::string client_command(const std::string& typed, const std::string& nick);

// The value of one tag of an IRCv3 message ("account=alice;msgid=1"), "" if it is not there
std::string tag_value(const std::string& tags, const std::string& name);

// The command with any password in it hidden, for logs and replies
std::string redact_command(const std::string& line);

// "warn,quiet,ban": the known steps of an escalation, in order
std::vector<std::string> parse_steps(const std::string& list);
