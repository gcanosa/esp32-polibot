// IRCClient: keeping a channel's topic and modes as they are locked
#include "irc_client.h"
#include "irc_internal.h"
#include "mode_lock.h"

// Two bots putting each other's changes back would never stop
bool IRCClient::may_revert(const std::string& channel) {
    const int most = g_settings.get()->lock_reverts;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return false;
        ChannelState& state = it->second;
        const int64_t now = seconds_now();
        if (now - state.reverts_since >= 60) {
            state.reverts_since = now;
            state.reverts = 0;
        }
        if (state.reverts > most) return false;
        if (state.reverts++ < most) return true;
    }
    report(ReportKind::channel, "The locked topic or modes of " + channel + " keep being changed. Leaving them for a minute.");
    return false;
}

void IRCClient::note_topic(const std::string& channel, const std::string& topic) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(irc_lower(channel));
    if (it != channels_.end()) it->second.topic = topic;
}

std::string IRCClient::current_topic(const std::string& channel) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(irc_lower(channel));
    return it == channels_.end() ? "" : it->second.topic;
}

void IRCClient::on_topic(const std::string& channel, const std::string& nick, const std::string& hostmask, const std::string& topic) {
    note_topic(channel, topic);
    ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    if (!rules.topic_lock || rules.topic == topic || same(nick, current_nick()) || is_report_channel(channel)) return;
    // an administrator's new topic is the one to keep from now on
    if (security_mgr_.has_permission(name_, nick, hostmask, "admin", channel)) {
        rules.topic = topic;
        guardian_.save_channel_config(name_, channel, rules);
        report(ReportKind::channel, nick + " changed the locked topic of " + channel + ": the new one is kept.");
        return;
    }
    if (can_act(channel) && may_revert(channel)) send_raw("TOPIC " + channel + " :" + rules.topic);
}

void IRCClient::keep_mode_lock(const std::string& channel, const std::string& by, const std::string& by_hostmask,
                               const std::vector<ModeChange>& changes) {
    const ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    if (rules.mode_lock.empty() || same(by, current_nick()) || !can_act(channel)) return;
    const std::string skip = rules.lockdown ? g_settings.get()->lockdown_modes : "";
    const std::string fix = broken_modes(rules.mode_lock, changes, skip);
    // administrators may do as they see fit
    if (fix.empty() || security_mgr_.has_permission(name_, by, by_hostmask, "admin", channel)) return;
    if (may_revert(channel)) send_raw("MODE " + channel + " " + fix);
}

void IRCClient::enforce_locks(const std::string& channel) {
    if (is_report_channel(channel) || !can_act(channel)) return;
    const ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    if (rules.topic_lock) {
        bool differs;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(channel));
            differs = it != channels_.end() && it->second.topic != rules.topic;
        }
        if (differs && may_revert(channel)) send_raw("TOPIC " + channel + " :" + rules.topic);
    }
    if (!rules.mode_lock.empty()) send_raw("MODE " + channel); // the reply shows what is missing
}

bool IRCClient::channel_snapshot(const std::string& channel, PsramVector<Member>& members, int& op, std::string& topic) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(irc_lower(channel));
    if (it == channels_.end()) return false;
    members = it->second.members.all();
    op = it->second.op;
    topic = it->second.topic;
    return true;
}
