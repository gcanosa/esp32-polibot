#include "bot_settings.h"
#include "rate_limits.h"
#include <esp_timer.h>
#include <algorithm>
#include <cstring>
#include <ctime>

namespace {

struct IntField {
    const char* section;
    const char* key;
    int BotSettings::*value;
    int min, max;
};
struct BoolField {
    const char* section;
    const char* key;
    bool BotSettings::*value;
};
struct TextField {
    const char* section;
    const char* key;
    std::string BotSettings::*value;
    const char* choices; // space separated; nullptr = free text
};

using S = BotSettings;
const IntField INTS[] = {
    {"limits", "send_burst", &S::send_burst, 1, 20},
    {"limits", "send_interval_ms", &S::send_interval_ms, 0, 10000},
    {"limits", "command_count", &S::command_count, 1, 100},
    {"limits", "command_seconds", &S::command_seconds, 1, 600},
    {"limits", "command_total", &S::command_total, 1, 120},
    {"limits", "login_attempts", &S::login_attempts, 1, LoginGuard::MAX_ATTEMPTS},
    {"limits", "login_window_minutes", &S::login_window_minutes, 1, 1440},
    {"limits", "login_block_minutes", &S::login_block_minutes, 1, 1440},
    {"limits", "masskick_max", &S::masskick_max, 1, 90},
    {"limits", "session_minutes", &S::session_minutes, 1, 1440},
    {"ops", "retry_seconds", &S::ops_retry_seconds, 30, 86400},
    {"ops", "rejoin_delay", &S::rejoin_delay, 1, 600},
    {"ops", "rejoin_max_delay", &S::rejoin_max_delay, 5, 86400},
    {"moderation", "ban_minutes", &S::ban_minutes, 0, 525600},
    {"moderation", "quiet_minutes", &S::quiet_minutes, 0, 525600},
    {"moderation", "strike_reset_minutes", &S::strike_reset_minutes, 1, 1440},
    {"moderation", "escalate_quiet_minutes", &S::escalate_quiet_minutes, 0, 525600},
    {"moderation", "escalate_ban_minutes", &S::escalate_ban_minutes, 0, 525600},
    {"raid", "join_count", &S::join_count, 2, 100},
    {"raid", "join_seconds", &S::join_seconds, 1, 600},
    {"raid", "unlock_after_seconds", &S::unlock_after_seconds, 0, 86400},
    {"raid", "clone_users", &S::clone_users, 2, 50},
    {"raid", "clone_seconds", &S::clone_seconds, 1, 600},
    {"raid", "clone_min_length", &S::clone_min_length, 1, 400},
    // the "seen" and "tell" ceilings keep both lists within the 512 KB settings partition
    {"updates", "check_hours", &S::update_check_hours, 1, 720},
    {"channel_limit", "interval_seconds", &S::limit_interval_seconds, 5, 600},
    {"channel_limit", "grace_seconds", &S::limit_grace_seconds, 0, 3600},
    {"channel_limit", "slack", &S::limit_slack, 0, 50},
    {"channel_limit", "lower_after_seconds", &S::limit_lower_after, 0, 86400},
    {"seen", "max_entries", &S::seen_max, 10, 500},
    {"seen", "keep_days", &S::seen_days, 1, 365},
    {"seen", "save_minutes", &S::seen_save_minutes, 1, 1440},
    {"tell", "per_recipient", &S::tell_per_recipient, 1, 20},
    {"tell", "per_sender", &S::tell_per_sender, 1, 50},
    {"tell", "max_total", &S::tell_max, 1, 150},
    {"tell", "keep_days", &S::tell_days, 1, 365},
    {"timed_voice", "max_remembered", &S::voice_max, 10, 2000},
    {"timed_voice", "keep_days", &S::voice_days, 1, 365},
    {"report", "max_per_minute", &S::report_per_minute, 1, 120},
    {"report", "low_memory_kb", &S::low_memory_kb, 0, 300},
};
const BoolField BOOLS[] = {
    {"updates", "auto_check", &S::update_auto_check},
    {"ops", "request", &S::request_ops},
    {"moderation", "exempt_channel_ops", &S::exempt_ops},
    {"seen", "enabled", &S::seen_enabled},
    {"seen", "save_message", &S::seen_text},
    {"tell", "enabled", &S::tell_enabled},
    {"report", "moderation", &S::report_moderation},
    {"report", "raid", &S::report_raid},
    {"report", "security", &S::report_security},
    {"report", "channel", &S::report_channel},
    {"report", "system", &S::report_system},
};
const TextField TEXTS[] = {
    {"moderation", "mask_style", &S::mask_style, "host userhost nick"},
    {"moderation", "escalation_steps", &S::escalation_steps, nullptr},
    {"raid", "join_action", &S::join_action, "off report lockdown"},
    {"raid", "lockdown_modes", &S::lockdown_modes, nullptr},
    {"raid", "clone_action", &S::clone_action, "off report kick quiet ban escalate lockdown"},
    {"tell", "delivery", &S::tell_delivery, "private notice channel"},
    {"help", "reply", &S::help_reply, "channel notice"},
};

bool is_choice(const char* choices, const std::string& value) {
    return !choices || (!value.empty() && value.find(' ') == std::string::npos &&
                        (" " + std::string(choices) + " ").find(" " + value + " ") != std::string::npos);
}

// "+" followed by mode letters only: it goes into a MODE line
bool valid_lockdown_modes(const std::string& s) {
    return s.size() >= 2 && s.size() <= 8 && s[0] == '+' &&
           std::all_of(s.begin() + 1, s.end(), [](unsigned char c) { return std::isalpha(c); });
}

cJSON* field(const cJSON* root, const char* section, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, section), key);
}

cJSON* section_of(cJSON* root, const char* name) {
    cJSON* section = cJSON_GetObjectItemCaseSensitive(root, name);
    return section ? section : cJSON_AddObjectToObject(root, name);
}

int clamp_number(const cJSON* v, const IntField& f) {
    // compare as double: a huge number must not wrap around
    return v->valuedouble < f.min ? f.min : v->valuedouble > f.max ? f.max : (int)v->valuedouble;
}

} // namespace

BotSettings parse_bot_settings(const cJSON* root) {
    BotSettings s;
    for (const auto& f : INTS) {
        const cJSON* v = field(root, f.section, f.key);
        if (cJSON_IsNumber(v)) s.*f.value = clamp_number(v, f);
    }
    for (const auto& f : BOOLS) {
        const cJSON* v = field(root, f.section, f.key);
        if (cJSON_IsBool(v)) s.*f.value = cJSON_IsTrue(v);
    }
    for (const auto& f : TEXTS) {
        const cJSON* v = field(root, f.section, f.key);
        if (cJSON_IsString(v) && is_choice(f.choices, v->valuestring)) s.*f.value = v->valuestring;
    }
    if (!valid_lockdown_modes(s.lockdown_modes)) s.lockdown_modes = BotSettings().lockdown_modes;
    return s;
}

void fill_bot_settings(cJSON* root) {
    const BotSettings s = parse_bot_settings(root);
    auto put = [&](const char* section, const char* key, cJSON* value) {
        cJSON* obj = section_of(root, section);
        cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
        cJSON_AddItemToObject(obj, key, value);
    };
    for (const auto& f : INTS) put(f.section, f.key, cJSON_CreateNumber(s.*f.value));
    for (const auto& f : BOOLS) put(f.section, f.key, cJSON_CreateBool(s.*f.value));
    for (const auto& f : TEXTS) put(f.section, f.key, cJSON_CreateString((s.*f.value).c_str()));
}

void sanitize_bot_settings(cJSON* posted) {
    const BotSettings s = parse_bot_settings(posted);
    // only sections and keys that were sent are touched
    auto replace = [&](const char* section, const char* key, cJSON* value) {
        cJSON* obj = cJSON_GetObjectItemCaseSensitive(posted, section);
        if (cJSON_GetObjectItemCaseSensitive(obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(obj, key, value);
        else cJSON_Delete(value);
    };
    for (const auto& f : INTS) replace(f.section, f.key, cJSON_CreateNumber(s.*f.value));
    for (const auto& f : BOOLS) replace(f.section, f.key, cJSON_CreateBool(s.*f.value));
    for (const auto& f : TEXTS) replace(f.section, f.key, cJSON_CreateString((s.*f.value).c_str()));
}

SettingsStore::SettingsStore(ConfigManager& config_mgr) : config_mgr_(config_mgr), current_(std::make_shared<BotSettings>()) {
    config_mgr_.register_rehash_listener([this](cJSON*) { load(); });
}

void SettingsStore::load() {
    cJSON* root = config_mgr_.get_root();
    auto next = std::make_shared<BotSettings>(parse_bot_settings(root));
    cJSON_Delete(root);
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = next;
}

std::shared_ptr<const BotSettings> SettingsStore::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

Clock bot_clock() {
    time_t now = 0;
    time(&now);
    return {esp_timer_get_time() / 1000000, now > 1700000000 ? (int64_t)now : 0};
}
