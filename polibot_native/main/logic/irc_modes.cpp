#include "irc_modes.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

void ServerSupport::apply(const std::vector<std::string>& tokens) {
    for (const auto& token : tokens) {
        if (token.rfind("PREFIX=(", 0) == 0) {
            const size_t close = token.find(')');
            if (close == std::string::npos) continue;
            const std::string modes = token.substr(8, close - 8);
            const std::string chars = token.substr(close + 1);
            if (modes.size() == chars.size()) {
                prefix_modes = modes;
                prefix_chars = chars;
            }
        } else if (token.rfind("MODES=", 0) == 0) {
            const int most = std::atoi(token.c_str() + 6);
            if (most > 0) max_modes = std::min(most, 20);
        } else if (token.rfind("CHANMODES=", 0) == 0) {
            std::istringstream groups(token.substr(10));
            std::string a, b, c;
            std::getline(groups, a, ',');
            std::getline(groups, b, ',');
            std::getline(groups, c, ',');
            list_modes = a;
            param_modes = b;
            set_param_modes = c;
        }
    }
}

char ServerSupport::mode_for_prefix(char symbol) const {
    const size_t at = prefix_chars.find(symbol);
    return at == std::string::npos ? 0 : prefix_modes[at];
}

std::vector<ModeChange> parse_modes(const ServerSupport& support, const std::vector<std::string>& args) {
    std::vector<ModeChange> changes;
    if (args.empty()) return changes;
    size_t next = 1;
    bool add = true;
    for (char mode : args[0]) {
        if (mode == '+' || mode == '-') {
            add = mode == '+';
            continue;
        }
        const bool takes_param = support.prefix_modes.find(mode) != std::string::npos ||
                                 support.list_modes.find(mode) != std::string::npos ||
                                 support.param_modes.find(mode) != std::string::npos ||
                                 (add && support.set_param_modes.find(mode) != std::string::npos);
        std::string param;
        if (takes_param && next < args.size()) param = args[next++];
        changes.push_back({add, mode, param});
    }
    return changes;
}
