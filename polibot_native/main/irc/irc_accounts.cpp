// IRCClient: who is signed in to services. The bot's users may be recognised by their account
// (SecurityManager), so it is only believed when the server will also say when it ends.
#include "irc_client.h"
#include "irc_internal.h"

bool IRCClient::learn_account(const std::string& nick, const std::string& userhost, const std::string& account) {
    return security_mgr_.note_account(name_, nick, userhost, account == "*" || account == "0" ? "" : account);
}

void IRCClient::forget_if_gone(const std::vector<std::string>& nicks) {
    std::vector<std::string> gone;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& nick : nicks) {
            bool here = false;
            for (auto& channel : channels_) here = here || channel.second.members.find(nick) != nullptr;
            if (!here) gone.push_back(nick);
        }
    }
    // unseen from now on: whoever takes the nickname later must prove who they are
    for (const auto& nick : gone) security_mgr_.note_account(name_, nick, "", "");
}

std::string IRCClient::account_status() {
    if (!registered_) return "not connected";
    return cap_account_tag_ || cap_account_notify_ ? "available" : "not offered by this server";
}
