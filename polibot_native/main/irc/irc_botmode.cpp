// IRCClient: marking the connection as a bot, where the admin wants it and the server offers it.
// Servers name the user mode in RPL_ISUPPORT ("BOT=B", IRCv3); older ones only list "B" among
// their user modes.
#include "irc_client.h"
#include "irc_internal.h"

void IRCClient::apply_bot_mode(bool turning_off) {
    std::string mode, me;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mode = !bot_letter_.empty() ? bot_letter_ : bot_listed_ ? "B" : "";
        me = current_nick_.empty() ? cfg_->nick : current_nick_;
        bot_mode_refused_ = false;
    }
    if (mode.empty()) {
        if (!turning_off) log("Bot mode: this server does not offer it.");
        return;
    }
    send_raw("MODE " + me + (turning_off ? " -" : " +") + mode);
}

// "+iwB" from RPL_UMODEIS or a MODE on our own nickname
void IRCClient::on_user_modes(const std::string& modes) {
    bool changed = false, set = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const char letter = bot_letter_.empty() ? 'B' : bot_letter_[0];
        bool adding = true;
        for (char c : modes) {
            if (c == '+' || c == '-') {
                adding = c == '+';
            } else if (c == letter && bot_mode_set_ != adding) {
                bot_mode_set_ = set = adding;
                changed = true;
            }
        }
    }
    if (changed) log(set ? "Bot mode is on." : "Bot mode is off.");
}

void IRCClient::on_bot_mode_refused() {
    if (!settings()->bot_mode) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (bot_mode_set_) return; // the refusal was about something else
        bot_mode_refused_ = true;
    }
    log("Bot mode: the server refused it.");
}

std::string IRCClient::bot_mode_status() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!cfg_->bot_mode) return "";
    if (!registered_) return "not connected";
    if (bot_mode_set_) return "on";
    if (bot_mode_refused_) return "refused by the server";
    return bot_letter_.empty() && !bot_listed_ ? "not offered by this server" : "waiting for the server";
}
