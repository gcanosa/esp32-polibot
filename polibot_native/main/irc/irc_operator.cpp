// IRCClient: joining again and getting operator status
#include "irc_client.h"
#include "irc_internal.h"
#include <algorithm>

static constexpr int OPS_REQUEST_GAP = 30;       // seconds between two requests for the same channel
static constexpr int GRANT_INTERVAL = 10;        // seconds between two looks for automatic modes

bool IRCClient::is_report_channel(const std::string& channel) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return !cfg_->report_channel.empty() && same(cfg_->report_channel, channel);
}

bool IRCClient::wanted_channel(const std::string& channel) {
    return is_report_channel(channel) || contains_ci(settings()->channels, channel);
}

std::string IRCClient::join_line(const std::string& channel) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!cfg_->report_channel.empty() && same(cfg_->report_channel, channel)) return "JOIN " + channel + " " + cfg_->report_key;
    auto key = cfg_->channel_keys.find(irc_lower(channel));
    return "JOIN " + channel + (key == cfg_->channel_keys.end() ? "" : " " + key->second);
}

// Each further attempt for the same channel waits twice as long
void IRCClient::schedule_join(const std::string& channel, int first_delay) {
    const int longest = std::max(g_settings.get()->rejoin_max_delay, first_delay);
    std::lock_guard<std::mutex> lock(state_mutex_);
    PendingJoin& pending = pending_joins_[irc_lower(channel)];
    pending.delay = pending.delay ? std::min(pending.delay * 2, longest) : first_delay;
    pending.name = channel;
    pending.at = seconds_now() + pending.delay;
}

void IRCClient::on_join_refused(const std::string& numeric, const std::string& channel) {
    if (!wanted_channel(channel)) return;
    const char* why = numeric == "471" ? "the channel is full"
                    : numeric == "473" ? "invite only"
                    : numeric == "474" ? "banned"
                    : numeric == "475" ? "wrong password"
                    : numeric == "477" ? "a registered nickname is needed"
                    : numeric == "403" ? "no such channel"
                    : numeric == "405" ? "too many channels"
                    : numeric == "489" ? "secure connections only"
                                       : "temporarily unavailable";
    const auto cfg = settings();
    if (numeric == "473" && !cfg->invite_command.empty()) send_raw(fill_template(cfg->invite_command, channel));
    if (numeric == "474" && !cfg->unban_command.empty()) send_raw(fill_template(cfg->unban_command, channel));
    bool first;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        PendingJoin& pending = pending_joins_[irc_lower(channel)]; // there already, after a kick
        first = !pending.refused;
        pending.refused = true;
        if (same(cfg_->report_channel, channel)) report_problem_ = std::string("cannot join: ") + why;
    }
    schedule_join(channel, g_settings.get()->rejoin_delay);
    if (first) report(ReportKind::channel, "Cannot join " + channel + ": " + why + ". Will keep trying.");
}

bool IRCClient::can_act(const std::string& channel) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channels_.find(irc_lower(channel));
    return it == channels_.end() || it->second.op != 0;
}

void IRCClient::note_operator(const std::string& channel, bool op, const std::string& by) {
    int before;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end()) return;
        before = it->second.op;
        it->second.op = op ? 1 : 0;
    }
    if (op && before != 1) {
        if (before == 0) report(ReportKind::channel, "I am an operator in " + channel + " now.");
        on_operator(channel);
    } else if (!op && before != 0) {
        if (before == 1) report(ReportKind::channel, "Lost operator status in " + channel + (by.empty() ? "." : " (by " + by + ")."));
        request_ops(channel);
    }
}

// Catches up on whatever needed operator status
void IRCClient::on_operator(const std::string& channel) {
    check_and_update_dynamic_limit(channel);
    if (is_report_channel(channel)) {
        send_raw("MODE " + channel); // the reply sets the password if needed
        return;
    }
    const auto settings = g_settings.get();
    const ChannelGuardianConfig rules = guardian_.get_channel_config(name_, channel);
    if (rules.lockdown) {
        send_raw("MODE " + channel + " " + settings->lockdown_modes);
        if (rules.auto_lockdown && settings->unlock_after_seconds) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(channel));
            // after a restart the countdown starts again, so the channel never stays locked for good
            if (it != channels_.end() && !it->second.unlock_at) it->second.unlock_at = seconds_now() + settings->unlock_after_seconds;
        }
    }
    sweep_akicks(channel);
    review_modes(channel);
    enforce_locks(channel);
}

void IRCClient::request_ops(const std::string& channel) {
    const auto cfg = settings();
    if (!g_settings.get()->request_ops || cfg->ops_command.empty()) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it == channels_.end() || seconds_now() - it->second.op_asked < OPS_REQUEST_GAP) return;
        it->second.op_asked = seconds_now();
    }
    log("Asking for operator status in " + channel);
    send_raw(fill_template(cfg->ops_command, channel));
}

void IRCClient::cannot_act(const std::string& channel, const std::string& what) {
    request_ops(channel);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channels_.find(irc_lower(channel));
        if (it != channels_.end()) {
            if (seconds_now() - it->second.complained < 60) return;
            it->second.complained = seconds_now();
        }
    }
    report(ReportKind::channel, "Cannot " + what + " in " + channel + ": I am not an operator there.");
}

void IRCClient::tick() {
    const int64_t now = seconds_now();
    if (now == last_tick_ || !registered_) return;
    last_tick_ = now;
    const auto settings = g_settings.get();
    const int retry = settings->ops_retry_seconds;
    std::vector<std::string> joins, deopped, unlocks, limits;
    std::vector<std::pair<std::string, bool>> looks; // channel, whether to ask about everybody
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (auto& entry : pending_joins_) {
            if (now < entry.second.at) continue;
            joins.push_back(entry.second.name);
            entry.second.at = now + entry.second.delay; // asked again if the server stays silent
        }
        for (auto& entry : channels_) {
            if (entry.second.limit_from >= 0 && now >= entry.second.limit_check) {
                limits.push_back(entry.second.name);
                entry.second.limit_check = now + settings->limit_interval_seconds;
            }
            if (entry.second.op == 0 && now - entry.second.op_asked >= retry) deopped.push_back(entry.second.name);
            if (entry.second.unlock_at && now >= entry.second.unlock_at && entry.second.op != 0) unlocks.push_back(entry.second.name);
            if (now >= entry.second.grant_check) {
                looks.push_back({entry.second.name, entry.second.review});
                entry.second.review = false;
                entry.second.grant_check = now + GRANT_INTERVAL;
            }
        }
    }
    for (const auto& channel : joins) send_raw(join_line(channel));
    for (const auto& channel : deopped) request_ops(channel);
    for (const auto& channel : unlocks) {
        release_lockdown(channel);
        report(ReportKind::raid, "The automatic lockdown of " + channel + " is over.");
    }
    for (const auto& channel : limits) check_and_update_dynamic_limit(channel);
    for (const auto& look : looks) grant_modes(look.first, "", look.second);
    lift_expired_bans();
    flush_reports();
}
