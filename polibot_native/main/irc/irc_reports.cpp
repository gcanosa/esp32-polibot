// IRCClient: the password protected report channel
#include "irc_client.h"
#include "irc_internal.h"


static constexpr int KEY_RETRY_GAP = 10; // seconds

// Reports are only sent once the server has shown that the channel needs the password
void IRCClient::check_report_key(bool has_key, const std::string& key_shown) {
    const auto cfg = settings();
    if (cfg->report_channel.empty()) return;
    // servers may show members "*" instead of the password
    const bool ours = has_key && (key_shown.empty() || key_shown == "*" || key_shown == cfg->report_key);
    bool greet = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        report_keyed_ = ours;
        if (ours) {
            report_problem_.clear();
            greet = !report_greeted_;
            report_greeted_ = true;
        }
    }
    if (ours) {
        if (greet) report(ReportKind::system, "Reporting here. " + software_description());
        flush_reports();
    } else if (can_act(cfg->report_channel)) {
        {
            // the server's own echoes of the change come through here too: do not repeat it for them
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (seconds_now() - report_key_set_at_ < KEY_RETRY_GAP) return;
            report_key_set_at_ = seconds_now();
        }
        if (has_key) send_raw("MODE " + cfg->report_channel + " -k " + (key_shown.empty() ? "*" : key_shown));
        send_raw("MODE " + cfg->report_channel + " +sk " + cfg->report_key);
    } else {
        std::lock_guard<std::mutex> lock(state_mutex_);
        report_problem_ = "waiting for operator status to set the password";
    }
}

void IRCClient::report(ReportKind kind, const std::string& text) {
    const char* tag = kind == ReportKind::moderation ? "[mod] "
                    : kind == ReportKind::raid ? "[raid] "
                    : kind == ReportKind::security ? "[security] "
                    : kind == ReportKind::channel ? "[channel] "
                                                  : "[system] ";
    const std::string line = tag + text;
    log("[report] " + line); // logged whether or not it is wanted on IRC
    const auto s = g_settings.get();
    const bool wanted = kind == ReportKind::moderation ? s->report_moderation
                      : kind == ReportKind::raid ? s->report_raid
                      : kind == ReportKind::security ? s->report_security
                      : kind == ReportKind::channel ? s->report_channel
                                                    : s->report_system;
    if (!wanted) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (cfg_->report_channel.empty()) return;
        const int64_t minute = seconds_now() / 60;
        if (minute != report_minute_) {
            if (reports_dropped_) reports_.push_dropping_oldest("[system] " + std::to_string(reports_dropped_) + " more reports were left out (too many).");
            report_minute_ = minute;
            reports_this_minute_ = 0;
            reports_dropped_ = 0;
        }
        if (++reports_this_minute_ > s->report_per_minute) {
            reports_dropped_++;
            return;
        }
        reports_.push_dropping_oldest(line.substr(0, 400));
    }
}

void IRCClient::flush_reports() {
    if (!registered_) return;
    for (;;) {
        std::string channel, line;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!report_keyed_ || cfg_->report_channel.empty() || !channels_.count(irc_lower(cfg_->report_channel))) return;
            channel = cfg_->report_channel;
            if (!reports_.pop(line)) return;
        }
        send_privmsg(channel, line);
    }
}

std::string IRCClient::report_status() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (cfg_->report_channel.empty()) return "";
    if (!registered_) return "not connected";
    if (!report_problem_.empty()) return report_problem_;
    if (!channels_.count(irc_lower(cfg_->report_channel))) return "joining";
    return report_keyed_ ? "ready" : "waiting for the server to confirm the password";
}
