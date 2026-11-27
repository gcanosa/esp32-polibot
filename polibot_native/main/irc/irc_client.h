#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cJSON.h>
#include <esp_tls.h>
#include <lwip/sockets.h>
#include "config_manager.h"
#include "security_manager.h"
#include "log_buffer.h"
#include "bot_settings.h"
#include "guardian.h"
#include "irc_config.h"
#include "auto_modes.h"
#include "irc_masks.h"
#include "irc_modes.h"
#include "irc_text.h"
#include "line_queue.h"
#include "rate_limits.h"
#include "floating_limit.h"
#include "members.h"
#include "outbox.h"

struct ChannelDetail {
    std::string network;
    std::string name;
    int users;
    int active_limit;
    int op; // -1 unknown, 0 not an operator, 1 operator
    bool dyn_enabled;
    int offset;
    bool entry_enabled;
    std::string entry_msg;
    bool lockdown;
};

struct ChannelState {
    std::string name; // as the server writes it
    MemberList members;
    int op = -1; // -1 unknown, 0 not an operator, 1 operator
    FloatingLimit limit;      // the +l the bot manages
    int64_t limit_from = -1;  // no +l before this; -1 until the member list is complete
    int64_t limit_check = 0;  // when to look at it next
    bool lift_limit = false;  // a limit from before the bot joined is still to be removed
    int split_away = 0;       // lost in a netsplit: still counted, so that they fit when they return
    int64_t split_until = 0;
    int64_t op_asked = -86400;   // when the services were last asked for operator status
    int64_t complained = -86400; // when a refused action was last reported
    int64_t unlock_at = 0;       // end of an automatic lockdown
    // Automatic modes and timed voice
    int64_t grant_check = 0; // when to look next
    bool review = true;      // ask about everybody at that look, not only those whose time is up
    bool moderated = false;  // +m: nobody new earns voice
    bool modes_asked = false;
    uint32_t paused_at = 0;  // since when the waiting time has stood still
    std::string topic;       // as the server has it
    int reverts = 0;         // of locked modes or topic, in the minute that began at reverts_since
    int64_t reverts_since = -60;
};

struct PendingJoin {
    std::string name;
    int64_t at = 0;
    int delay = 0;
    bool refused = false; // the reason has been reported
};

// What a report is about; each kind can be switched off
enum class ReportKind { moderation, raid, security, channel, system };

// Human readable build and hardware description (CTCP VERSION, !version)
std::string software_description();

// One network connection, running in its own task. Its source files, by subject:
//   irc_client      life cycle           irc_operator    operator status, joining again
//   irc_connection  sockets and TLS      irc_protection  kicks, bans, lockdowns
//   irc_registration sign-in, nickname   irc_reports     report channel
//   irc_dispatch    incoming lines       irc_recall      seen and tell
//   irc_channels    channel members      irc_botmode     bot user mode
//   irc_accounts    services accounts     irc_automodes   auto-op, auto-voice, timed voice
//   irc_locks       topic and mode locks
//   commands*       IRC commands
class IRCClient : public std::enable_shared_from_this<IRCClient> {
public:
    IRCClient(ConfigManager& config_mgr, SecurityManager& security_mgr, GuardianEngine& guardian, const IrcSettings& settings);
    ~IRCClient();

    bool start();
    void stop();                              // disconnects and ends the task
    void reconnect() { reconnect_ = true; }
    void retry_now() { wake_ = true; }        // cuts a back-off wait short; leaves a live connection alone
    void apply(const IrcSettings& settings);  // reconnects only if connection settings changed

    // Sends a line, or queues it so that the server does not drop the bot for flooding.
    // in_order: it queues with the channel actions, so that nothing queued after it overtakes it
    bool send_raw(const std::string& line, bool in_order = false);
    bool send_privmsg(const std::string& target, const std::string& message);
    bool send_notice(const std::string& target, const std::string& message);

    bool check_and_update_dynamic_limit(const std::string& channel); // true if a MODE line went out
    void execute_action(const GuardianAction& act);
    void handle_command(const std::string& target, const std::string& sender_nick, const std::string& hostmask, const std::string& cmd_string, bool is_pm);
    void lockdown(const std::string& channel, bool automatic);
    void release_lockdown(const std::string& channel);
    void review_modes(const std::string& channel = "");  // "" = all; the connection task does the rest
    std::string access_who(const std::string& typed);    // what a list entry stores for what was typed
    void devoice_restricted(const std::string& channel, const std::string& change); // before a ban or a mute
    void enforce_locks(const std::string& channel); // puts back a locked topic, and asks for the modes to check
    std::string current_topic(const std::string& channel);
    // Copies of who is there and what is known; false if the bot is not in the channel
    bool channel_snapshot(const std::string& channel, PsramVector<Member>& members, int& op, std::string& topic);
    bool is_mute(const std::string& mode); // a recorded "+x ..." change that this network uses to mute
    std::pair<bool, std::string> lift_ban(const std::string& channel, const std::string& mode); // one the bot placed

    // Logs the event and tells the report channel, if there is one
    void report(ReportKind kind, const std::string& text); // waits for the connection task to send it
    void flush_reports();                                   // sends what waits, if the channel is ready
    std::string report_status();   // "" when no report channel is set, "ready" when it works
    std::string account_status();  // whether the server says who is signed in to services
    std::string bot_mode_status(); // "" when not wanted

    // Getters for status & web panel
    const std::string& name() const { return name_; }
    std::shared_ptr<const IrcSettings> settings(); // shared, never copied: they change as a whole
    std::string current_nick();
    bool is_connected() const { return connected_; }
    bool is_registered() const { return registered_; } // the server has welcomed the bot
    std::vector<std::string> get_joined_channels();    // without the report channel
    std::vector<ChannelDetail> get_channels_detail();  // without the report channel

private:
    ConfigManager& config_mgr_;
    SecurityManager& security_mgr_;
    GuardianEngine& guardian_;
    const std::string name_;
    std::shared_ptr<const IrcSettings> cfg_; // guarded by state_mutex_

    // Sockets & TLS: owned by the connection task; io_mutex_ serialises reads, writes and close
    int sock_fd_ = -1;
    esp_tls_t* tls_handle_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> registered_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> reconnect_{false};
    std::atomic<bool> wake_{false};
    int watch_ = -1; // the connection task's place in the hang check
    int64_t start_time_ = 0;
    int64_t last_activity_ = 0;
    int64_t last_ctcp_reply_ = -100;
    int64_t last_tick_ = 0;

    Outbox outbox_; // lines waiting their turn (guarded by io_mutex_)

    // State (guarded by state_mutex_)
    std::string current_nick_; // as the server knows us; empty while disconnected
    // Capabilities and signing in during registration; only touched by the connection task
    int64_t cap_deadline_ = 0; // seconds; 0 = no negotiation in progress
    int cap_requests_ = 0;     // requests the server has yet to answer
    bool sasl_ok_ = false;
    bool cap_extended_join_ = false;
    bool whox_ = false;        // the server can tell the accounts of those already in a channel
    std::atomic<bool> cap_account_tag_{false};    // every message names its sender's account
    std::atomic<bool> cap_account_notify_{false}; // sign-ins and sign-outs are announced
    int nick_attempts_ = 0; // replacements tried during this registration
    std::string server_caps_;
    ServerSupport support_;
    std::map<std::string, ChannelState> channels_;     // the joined ones, by lower case name
    std::map<std::string, PendingJoin> pending_joins_; // to be joined again
    ExpiringSet netsplit_{512};                        // nicknames lost in a netsplit
    RateWindow commands_{256};                         // from people without a role (two entries each)
    // Bot mode: the letter the server names for it, and whether it is set on us
    std::string bot_letter_;
    bool bot_listed_ = false; // 'B' is among the server's user modes
    bool bot_mode_set_ = false;
    bool bot_mode_refused_ = false;
    // Report channel
    bool report_keyed_ = false; // the server confirmed the password is set
    bool report_greeted_ = false;
    int64_t report_key_set_at_ = -60; // when the bot last tried to set the password
    std::string report_problem_;
    LineQueue reports_{4096};
    int64_t report_minute_ = 0;
    int reports_this_minute_ = 0;
    int reports_dropped_ = 0;
    // A masskick waits for the server's member list (WHO) so it can judge each user by their hostmask
    struct MassKick {
        bool active = false;
        std::string channel, pattern, reason, reply_target;
        std::vector<std::string> victims;
        size_t skipped = 0; // matched beyond the per-command limit
    } masskick_;
    std::mutex state_mutex_;
    std::mutex io_mutex_;

    static void task_entry(void* pv);
    void run_loop();
    bool connect_once();
    bool connect_socket(int fd, const struct sockaddr* addr, socklen_t len);
    void pause_ms(int ms); // returns early when asked to stop or reconnect
    void try_another_nick(const IrcSettings& cfg, const std::string& refused);
    bool capability_command(const std::string& command, const std::vector<std::string>& args, const std::string& trailing);
    void request_capabilities();
    void close_request(const std::string& why); // one is answered: registration goes on after the last
    void send_sasl_credentials();
    void run_perform(const IrcSettings& cfg); // the network's own commands, after the welcome
    void end_negotiation(const std::string& why);
    bool write_locked(const std::string& line);
    void flush_queue();
    void read_loop();
    void close_connection();
    void process_raw_line(const std::string& raw_line);
    void on_message(const std::string& command, const std::string& target, const std::string& nick, const std::string& hostmask, const std::string& message);
    void log(const std::string& msg);

    // Bot mode
    void apply_bot_mode(bool turning_off = false);
    void on_user_modes(const std::string& modes);
    void on_bot_mode_refused();

    // Channel members
    bool channel_event(const std::string& command, const std::vector<std::string>& args, const std::string& trailing,
                       const std::string& nick, const std::string& hostmask, bool from_me);
    void on_join(const std::string& channel, const std::string& nick, const std::string& hostmask, bool from_me);
    void on_kick(const std::string& channel, const std::string& victim, const std::string& by, const std::string& reason, bool me);
    void on_quit_or_nick(bool quit, const std::string& nick, const std::string& hostmask, const std::string& detail, bool from_me);
    void on_mode(const std::string& channel, const std::vector<std::string>& modes, const std::string& by, const std::string& by_hostmask);
    void on_names(const std::string& channel, const std::string& names);
    void on_who(const std::string& channel, const std::string& userhost, const std::string& nick, const std::string& flags);

    // Automatic modes (irc_automodes.cpp)
    void grant_modes(const std::string& channel, const std::string& only, bool everyone);
    void grant_everywhere(const std::string& nick); // services say who they are
    void note_voice(const std::string& channel, const PsramVector<Member>& people, bool lost);
    void on_channel_modes(const std::string& channel, const std::vector<std::string>& modes); // 324

    // Topic and mode locks (irc_locks.cpp)
    void note_topic(const std::string& channel, const std::string& topic);
    void on_topic(const std::string& channel, const std::string& nick, const std::string& hostmask, const std::string& topic);
    void keep_mode_lock(const std::string& channel, const std::string& by, const std::string& by_hostmask, const std::vector<ModeChange>& changes);
    bool may_revert(const std::string& channel); // false once a channel has been put back too often

    // Who is signed in to services (irc_accounts.cpp)
    bool learn_account(const std::string& nick, const std::string& userhost, const std::string& account);
    void forget_if_gone(const std::vector<std::string>& nicks); // those in none of the channels any more

    // Operator status and joining
    void tick(); // once a second: delayed joins, expiring bans, waiting reports
    bool is_report_channel(const std::string& channel);
    bool wanted_channel(const std::string& channel);
    std::string join_line(const std::string& channel);
    void schedule_join(const std::string& channel, int first_delay);
    void on_join_refused(const std::string& numeric, const std::string& channel);
    bool can_act(const std::string& channel); // false only when the bot knows it is not an operator
    void note_operator(const std::string& channel, bool op, const std::string& by);
    void on_operator(const std::string& channel);
    void request_ops(const std::string& channel);
    void cannot_act(const std::string& channel, const std::string& what);

    // Protection
    bool moderate(const std::string& channel, const std::string& nick, const std::string& hostmask, const std::string& message);
    LineContext line_context(const std::string& channel, const std::string& nick, const std::string& message, bool& is_operator);
    void note_presence(const std::string& channel, const std::string& nick, const std::string& hostmask, bool joined);
    std::string member_userhost(const std::string& nick); // from the channels, else from "seen"
    std::string mask_for(const std::string& target);
    void raid_lockdown(const GuardianAction& act);
    __attribute__((noinline)) void remove_or_mute(const GuardianAction& act);
    void sweep_akicks(const std::string& channel);
    void lift_expired_bans();
    void finish_masskick(const std::string& channel);

    // Report channel
    void check_report_key(bool has_key, const std::string& key_shown);

    // Seen and tell
    void note_seen(const std::string& nick, const std::string& hostmask, char action, const std::string& channel, const std::string& text);
    void deliver_tells(const std::string& nick, const std::string& channel);

    // Commands
    struct Command {
        std::string name; // lower case, without the prefix
        std::vector<std::string> args;
        std::string target, nick, hostmask, reply_to;
        bool is_pm = false;
        bool audit = false; // it changed something: the report channel hears about it
    };
    // One function per command group: a single one needed more stack than the calling tasks can spare.
    // Each returns true once it has dealt with the command.
    __attribute__((noinline)) bool session_command(Command& c);
    __attribute__((noinline)) bool info_command(Command& c);
    __attribute__((noinline)) bool seen_command(Command& c);
    __attribute__((noinline)) bool tell_command(Command& c);
    __attribute__((noinline)) bool akick_command(Command& c);
    __attribute__((noinline)) bool status_command(Command& c);
    __attribute__((noinline)) bool moderator_command(Command& c);
    __attribute__((noinline)) bool ban_command(Command& c);
    __attribute__((noinline)) bool bans_command(Command& c);
    __attribute__((noinline)) bool password_command(Command& c);
    __attribute__((noinline)) bool lock_command(Command& c);
    __attribute__((noinline)) bool channel_command(Command& c);
    __attribute__((noinline)) bool access_command(Command& c);
    __attribute__((noinline)) bool timevoice_command(Command& c);
    __attribute__((noinline)) bool dynlimit_command(Command& c);
    __attribute__((noinline)) bool lockdown_command(Command& c);
    __attribute__((noinline)) bool entrymsg_command(Command& c);
    __attribute__((noinline)) bool badwords_command(Command& c);
    __attribute__((noinline)) bool autojoin_command(Command& c);
    __attribute__((noinline)) bool owner_command(Command& c);
    __attribute__((noinline)) bool masskick_command(Command& c);
    __attribute__((noinline)) bool admin_command(Command& c);
    __attribute__((noinline)) bool ota_command(Command& c);
    bool set_autojoin(const std::string& channel, bool wanted, const std::string& key);
    bool admit_command(const Command& c);
    void confirm(Command& c, const std::string& channel, const std::string& done);
    int help_level(const Command& c);
    bool allowed(const Command& c, const char* role, const std::string& channel = "");
    void resolve_channel(Command& c, std::string& channel);
};

// Keeps one IRCClient per configured network and applies configuration changes to them
class IRCManager {
public:
    IRCManager(ConfigManager& config_mgr, SecurityManager& security_mgr, GuardianEngine& guardian);

    void start(); // once config.json is loaded
    void apply_config(); // connects, changes and disconnects networks to match the configuration
    void network_up(bool ip_changed); // WiFi (re)connected
    void reconnect_all();
    void report(ReportKind kind, const std::string& text); // to every network's report channel
    std::vector<std::shared_ptr<IRCClient>> clients();
    std::shared_ptr<IRCClient> find(const std::string& network); // "" = first network
    int64_t get_start_time() const { return start_time_; }

private:
    ConfigManager& config_mgr_;
    SecurityManager& security_mgr_;
    GuardianEngine& guardian_;
    std::vector<std::shared_ptr<IRCClient>> clients_;
    std::mutex mutex_;
    std::atomic<bool> started_{false};
    int64_t start_time_ = 0;
};

extern IRCManager g_irc_manager;
