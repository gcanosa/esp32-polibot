#pragma once

// Bot behaviour settings. Every field is described once, in the table in bot_settings.cpp, which
// reads it from config.json, keeps it in range, and gives the web panel its current value.
#include <memory>
#include <mutex>
#include <string>
#include <cJSON.h>
#include "bot_clock.h"
#include "config_manager.h"

struct BotSettings {
    // "limits"
    int send_burst = 5;
    int send_interval_ms = 1000;
    int command_count = 5;
    int command_seconds = 10;
    int command_total = 20;
    int login_attempts = 5;
    int login_window_minutes = 10;
    int login_block_minutes = 10;
    int masskick_max = 50;
    int session_minutes = 30; // an idle web panel session ends after this
    // "ops"
    bool request_ops = true;
    int ops_retry_seconds = 300;
    int rejoin_delay = 5;
    int rejoin_max_delay = 300;
    // "moderation"
    bool exempt_ops = true;
    std::string mask_style = "host";
    int ban_minutes = 0;
    int quiet_minutes = 0;
    std::string escalation_steps = "warn,quiet,ban";
    int strike_reset_minutes = 10;
    int escalate_quiet_minutes = 5;
    int escalate_ban_minutes = 60;
    // "raid"
    std::string join_action = "report";
    int join_count = 8;
    int join_seconds = 10;
    std::string lockdown_modes = "+mi";
    int unlock_after_seconds = 180;
    std::string clone_action = "report";
    int clone_users = 4;
    int clone_seconds = 15;
    int clone_min_length = 12;
    // "channel_limit": the floating +l (free places and the lowest value are set per channel)
    int limit_interval_seconds = 30;
    int limit_grace_seconds = 180; // no limit for this long after joining a channel
    int limit_slack = 2;
    int limit_lower_after = 300;
    // "help"
    std::string help_reply = "channel"; // asked in a channel: "channel" answers there, "notice" the asker only
    // "seen"
    bool seen_enabled = true;
    bool seen_text = true;
    int seen_max = 300;
    int seen_days = 30;
    int seen_save_minutes = 10;
    // "tell"
    bool tell_enabled = true;
    std::string tell_delivery = "private";
    int tell_per_recipient = 5;
    int tell_per_sender = 5;
    int tell_max = 100;
    int tell_days = 30;
    // "timed_voice": the memory of it (the waiting time is set per channel)
    int voice_max = 500;
    int voice_days = 90; // forgotten after this long without being seen
    // "report"
    bool report_moderation = true;
    bool report_raid = true;
    bool report_security = true;
    bool report_channel = true;
    bool report_system = true;
    int report_per_minute = 20;
    int low_memory_kb = 20; // 0 = never report
};

BotSettings parse_bot_settings(const cJSON* root);
// Adds the value in effect for every setting that config.json leaves out
void fill_bot_settings(cJSON* root);
// Brings posted settings into range, so that what is stored is what the bot uses
void sanitize_bot_settings(cJSON* posted);

class SettingsStore {
public:
    explicit SettingsStore(ConfigManager& config_mgr);
    void load();
    std::shared_ptr<const BotSettings> get() const;

private:
    ConfigManager& config_mgr_;
    mutable std::mutex mutex_;
    std::shared_ptr<const BotSettings> current_;
};

extern SettingsStore g_settings;

// Now, on both of the bot's clocks
Clock bot_clock();
