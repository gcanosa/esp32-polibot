// Runs on a PC, from the project folder:
// c++ -std=c++17 -I main/irc test/command_help_test.cpp main/irc/command_help.cpp -o /tmp/command_help_test && /tmp/command_help_test
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include "command_help.h"

static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
static std::vector<std::string> help(const std::string& topic, int level = 100, const std::string& prefix = "!") {
    return help_text(topic, prefix, "Bot", level, "9.9.9");
}

int main() {
    // every command the bot handles has an explanation and an example
    std::stringstream source;
    for (const auto& entry : std::filesystem::directory_iterator("main/irc")) { // run from the project folder
        if (entry.path().filename().string().rfind("commands", 0) == 0) source << std::ifstream(entry.path()).rdbuf();
    }
    const std::string code = source.str();
    std::regex pattern("(?:cmd|c\\.name) [!=]= \"([a-z]+)\"");
    int commands = 0;
    for (std::sregex_iterator it(code.begin(), code.end(), pattern), end; it != end; ++it, ++commands) {
        const std::string name = (*it)[1];
        auto lines = help(name);
        if (lines.size() != 2) std::printf("missing help for '%s'\n", name.c_str());
        assert(lines.size() == 2 && has(lines[1], "Example: "));
    }
    assert(commands > 30);

    // the list follows the role
    auto user = help("", 10);
    assert(user.size() == 2 && has(user[0], "your role: everyone") && has(user[1], "!chanstats") && !has(user[1], "!kick"));
    const auto aloud = help_text("", "!", "Bot", 0, "9.9.9"); // said in a channel
    assert(aloud.size() == 2 && !has(aloud[0], "your role") && has(aloud[0], "in private") && has(aloud[1], "!chanstats") && !has(aloud[1], "!kick"));
    assert(help("", 50).size() == 3 && help("", 75).size() == 4 && help("", 100).size() == 5);
    assert(has(help("", 100).back(), "!masskick"));

    // configured prefix everywhere
    for (const auto& line : help("", 100, ".")) assert(!has(line, "!"));
    assert(has(help("ban", 100, ".")[0], ".ban [#chan]") && has(help("ban", 100, ".")[1], "Example: .ban "));
    assert(help(".ban", 100, ".") == help("ban", 100, "."));
    assert(help("!BAN") == help("ban"));
    assert(help("limit") == help("dynlimit") && help("auth") == help("login"));
    assert(has(help("login")[1], "/msg Bot login mypassword"));
    assert(help("nothing").size() == 1 && has(help("nothing")[0], "!help"));
    assert(help("up").size() == 1 && help("!").size() == 1); // no partial matches

    // each line fits in one IRC message
    for (const auto& topic : {"", "admin", "ban", "login", "dynlimit"}) {
        for (const auto& line : help(topic)) assert(line.size() < 400);
    }
    for (const auto& line : help(std::string(500, 'x'))) assert(line.size() < 400);
    std::puts("command help: all checks passed");
}
