#pragma once

// What the server supports (RPL_ISUPPORT) and how its channel mode lines read
#include <string>
#include <vector>

struct ServerSupport {
    std::string prefix_modes = "ov";
    std::string prefix_chars = "@+";
    std::string list_modes = "beIq";   // always take a parameter
    std::string param_modes = "k";     // always take a parameter
    std::string set_param_modes = "l"; // take one only when being set
    int max_modes = 3;                 // changes with a parameter in one MODE line

    void apply(const std::vector<std::string>& tokens);
    char mode_for_prefix(char symbol) const; // 0 if the character is not a member prefix
};

struct ModeChange {
    bool add;
    char mode;
    std::string param;
};

// args: the mode letters first ("+o-v"), then their parameters
std::vector<ModeChange> parse_modes(const ServerSupport& support, const std::vector<std::string>& args);
