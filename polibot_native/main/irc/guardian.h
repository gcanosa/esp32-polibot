#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <cJSON.h>
#include "config_manager.h"
#include "security_manager.h"
#include "access_list.h"
#include "bot_settings.h"
#include "irc_masks.h"
#include "irc_text.h"
#include "rate_limits.h"

struct GuardianAction {
    // "kick", "ban", "quiet", "warn", "report", "lockdown", "akick", "mode", "notice", "privmsg"
    std::string type;
    std::string channel;
    std::string nick;
    std::string reason;
    std::string mode; // for "akick": the mask to ban
    std::string target;
    std::string message;
    int seconds = 0; // how long a ban or quiet lasts; 0 = until lifted
};

struct DynamicLimitConfig {
    bool enabled = true;
    int offset = 3;
    int min_limit = 5;
};

struct EntryMsgConfig {
    bool enabled = true;
    bool notice = true;
    std::string message;
};

// Voice for those who have been in the channel a while
struct TimedVoiceConfig {
    bool enabled = false;
    int minutes = 10; // how long somebody has to be there first
};

constexpr int MAX_VOICE_MINUTES = 365 * 1440;

struct ChannelGuardianConfig {
    DynamicLimitConfig dynamic_limit;
    EntryMsgConfig entrymsg;
    std::vector<std::string> badwords;
    bool lockdown = false;
    bool auto_lockdown = false; // set by raid protection, so it ends by itself
    TimedVoiceConfig timed_voice;
    int auto_op = -1; // opping admins in this channel: -1 as the general setting says, 0 no, 1 yes
    bool topic_lock = false; // the topic below is put back when somebody else changes it
    std::string topic;
    std::string mode_lock; // modes kept set or unset, like "+nt-s"; "" = none
};

// What the connection knows about a line and its sender
struct LineContext {
    bool voiced = false;
    int named = 0; // how many of those present it names
};

// What the bot gives out by itself in one channel
struct AutoModeRules {
    int voice_minutes = 0; // 0 = no timed voice
    bool admins = false;   // admins are opped
    bool listed = false;   // the channel has entries in the access lists
    bool lockdown = false;
};

// Who is removed on sight. An empty network or channel means all of them.
struct AkickEntry {
    std::string network;
    std::string channel;
    std::string mask;
    std::string reason;
};

constexpr size_t MAX_AKICKS = 100;

class GuardianEngine {
public:
    GuardianEngine(ConfigManager& config_mgr, SecurityManager& security_mgr);

    void load_rules();
    void on_rehash(cJSON* root);

    // Rules are stored as "network/#channel"; a plain "#channel" entry applies to every network without its own
    ChannelGuardianConfig get_channel_config(const std::string& network, const std::string& channel);
    void save_channel_config(const std::string& network, const std::string& channel, const ChannelGuardianConfig& cfg);
    std::vector<std::string> get_global_badwords();
    AutoModeRules auto_modes(const std::string& network, const std::string& channel); // nothing is copied

    // The auto-op, half-op and voice lists (guardian_access.cpp). Changes are saved at once.
    uint8_t listed_modes(const std::string& network, const std::string& channel, const std::string& hostmask,
                         const std::string& account, const std::string& user);
    std::vector<std::string> access_entries(const std::string& network, const std::string& channel, char mode);
    // Empty when done, otherwise the reason it was not
    std::string add_access(const std::string& network, const std::string& channel, char mode, const std::string& who);
    bool remove_access(const std::string& network, const std::string& channel, char mode, const std::string& who);

    std::vector<AkickEntry> get_akicks();
    bool akick_match(const std::string& network, const std::string& channel, const std::string& hostmask, AkickEntry& out);
    // Adds or replaces; false if the list is full or could not be saved
    bool add_akick(const AkickEntry& entry);
    // Removes the entries of this network and channel ("" = the all-channel ones) with this mask
    bool remove_akick(const std::string& network, const std::string& channel, const std::string& mask);

    std::vector<GuardianAction> inspect_message(const std::string& network, const std::string& channel, const std::string& nick, const std::string& hostmask,
                                                const std::string& message, const LineContext& line);
    // count_for_raid is false for users coming back from a netsplit
    std::vector<GuardianAction> on_user_join(const std::string& network, const std::string& channel, const std::string& nick, const std::string& hostmask, bool count_for_raid);
    // Floods by one user. A nickname change is judged in every channel given; a join, part or quit in
    // one channel, where something is only done on a join, while they are there to be removed.
    std::vector<GuardianAction> on_nick_change(const std::string& network, const std::vector<std::string>& channels, const std::string& nick, const std::string& hostmask);
    std::vector<GuardianAction> on_presence(const std::string& network, const std::string& channel, const std::string& nick, const std::string& hostmask, bool joined);

private:
    ConfigManager& config_mgr_;
    SecurityManager& security_mgr_;
    std::mutex mutex_;

    // Global settings
    bool flood_enabled_ = true;
    int flood_max_lines_ = 4;
    int flood_time_window_ = 4;
    std::string flood_action_ = "kick";
    std::string flood_kick_reason_ = "Flood protection triggered";

    bool caps_enabled_ = false;
    int caps_min_length_ = 15;
    int caps_threshold_ = 75;
    std::string caps_action_ = "warn";
    std::string caps_kick_reason_ = "Excessive caps detected";

    bool badwords_enabled_ = true;
    std::vector<std::string> global_badwords_;
    std::string badwords_action_ = "kick";
    std::string badwords_kick_reason_ = "Forbidden term detected";

    bool auto_op_admins_ = true;

    // Per-channel configs
    std::map<std::string, ChannelGuardianConfig> channel_settings_;
    std::vector<AkickEntry> akicks_;
    AccessList access_;
    std::mutex access_saving_; // one change of the lists at a time

    // Message history tracking
    RateWindow messages_{256};
    RateWindow joins_{128};
    CloneDetector clones_;
    RateWindow churn_{256}; // nickname changes, joins and parts
    Strikes strikes_;

    // The stored rules that apply, or null for the defaults
    const ChannelGuardianConfig* find_channel_locked(const std::string& network, const std::string& channel) const;
    void load_access_locked(const cJSON* guardian);
    bool save_access(const PsramVector<AccessEntry>& entries); // call without holding mutex_
    // The action a rule asks for, with "escalate" turned into this offender's next step
    GuardianAction verdict_locked(const std::string& action, const std::string& reason, const std::string& channel,
                                  const std::string& nick, const std::string& offender, const BotSettings& settings);
};

extern GuardianEngine g_guardian;
