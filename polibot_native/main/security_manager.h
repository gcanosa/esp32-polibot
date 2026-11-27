#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>
#include <cJSON.h>
#include "config_manager.h"
#include "irc_masks.h" // wildcard_match
#include "irc_text.h"
#include "rate_limits.h"
#include "account_book.h"

int get_role_level(const std::string& role);

// Contains a space, which an IRC prefix never can: only the authenticated web panel uses it.
inline constexpr const char* WEB_CONSOLE_HOSTMASK = "web console";

// Ships with every image, so it only serves to choose another one in the web panel
inline constexpr const char* DEFAULT_PASSWORD = "polibotadmin";
constexpr size_t MAX_WEB_SESSIONS = 8;

struct UserAccount {
    std::string username;
    std::string salt;
    std::string password_hash;
    std::string global_role;
    std::map<std::string, std::string> channel_roles;
    std::vector<std::string> hostmasks;
    std::vector<std::string> accounts; // services accounts that are this person, as "network/account"
    bool default_password = false;     // still the password every image ships with
};

// What the bot knows about somebody on IRC
struct Identity {
    std::string account; // services account, "" if not signed in or unknown
    std::string user;    // the bot's account they are identified as, "" if none
    int level = 0;       // their role in the channel asked about
};

struct IrcSession {
    std::string username;
    std::string hostmask;
    std::string network;
    std::string nick;
};

struct WebSession {
    std::string username;
    int64_t last_used; // microseconds since boot, which also orders them; sessions left idle expire
    uint32_t id;       // names the session in the panel, which never sees another one's token
    std::string address;
};

// Somebody signed in, as the panel shows it
struct SessionView {
    std::string id;      // "w<number>" or "i<network> <nick>"
    std::string user;
    std::string network; // "" for the web panel
    std::string nick;
    std::string from;    // hostmask, or the browser's address
    int idle = -1;       // seconds, web sessions only
    bool current = false;
};

class SecurityManager {
public:
    explicit SecurityManager(ConfigManager& config_mgr);

    void load();
    void on_rehash(cJSON* root);

    // Crypto & Hashing
    static std::string generate_salt(size_t hex_len = 8);
    static std::string compute_sha256_hex(const std::string& input);
    static bool hash_password(const std::string& password, std::string& salt, std::string& out_hash);
    static bool verify_password(const std::string& password, const std::string& salt, const std::string& expected_hash);

    // Authentication & Sessions
    std::pair<bool, std::string> irc_login(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& password, const std::string& username = "");
    std::pair<bool, std::string> irc_logout(const std::string& network, const std::string& nick);
    // The account's owner changes its password over IRC: the current one is needed, and wrong ones count
    std::pair<bool, std::string> change_password_on_irc(const std::string& network, const std::string& hostmask, const std::string& username,
                                                        const std::string& current, const std::string& next);
    std::string get_role(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& channel = "");
    bool has_permission(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& required_role, const std::string& channel = "");
    bool get_effective_user(const std::string& network, const std::string& nick, const std::string& hostmask, UserAccount& out_user);

    Identity identify(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& channel);

    // What the server says about who is signed in to services; an empty account forgets the user.
    // True if it tells something new about who they are.
    bool note_account(const std::string& network, const std::string& nick, const std::string& userhost, const std::string& account);
    void rename_account(const std::string& network, const std::string& from, const std::string& to);
    void forget_accounts(const std::string& network);

    // Failed passwords are counted per source ("web <address>"); too many pause that source
    int login_blocked_for(const std::string& source); // seconds left, 0 = may try
    bool login_failed(const std::string& source);     // true if the source is paused from now on

    // True only after a password login from this exact hostmask (no hostmask-only trust)
    bool has_owner_session(const std::string& network, const std::string& nick, const std::string& hostmask);

    // Web Auth
    bool authenticate(const std::string& username, const std::string& password, UserAccount& out_user);
    std::string create_web_session(const std::string& username, const std::string& address);
    // counts_as_activity is false for the panel's own refreshing, which must not keep a session alive
    bool validate_web_session(const std::string& token, UserAccount& out_user, bool counts_as_activity = true);
    void destroy_web_session(const std::string& token);

    // Who is signed in, on IRC and in the panel
    std::vector<SessionView> sessions(const std::string& current_token);
    // Signs somebody out. Empty if done (`username` then says who), otherwise why not.
    std::string end_session(const std::string& id, bool by_owner, std::string& username);

    // User management
    // Returns {saved, message}. An empty password, role or mask list keeps what the account has; channel
    // roles and services accounts are replaced whenever a list is given, also by an empty one.
    // A new password signs the account out everywhere except the web session `keep_token`.
    std::pair<bool, std::string> add_or_update_user(const std::string& username, const std::string& password, const std::string& global_role = "", const std::map<std::string, std::string>* channel_roles = nullptr, const std::vector<std::string>& hostmasks = {}, const std::string& keep_token = "", const std::vector<std::string>* accounts = nullptr);
    std::pair<bool, std::string> set_password(const std::string& username, const std::string& password, const std::string& keep_token = "");
    bool remove_user(const std::string& username, std::string& out_msg);
    std::vector<UserAccount> list_users();

    bool persist_users();

private:
    ConfigManager& config_mgr_;
    std::vector<UserAccount> users_;
    std::map<std::string, IrcSession> irc_sessions_;  // "network nick" (lower case) -> session
    std::map<std::string, WebSession> web_sessions_;  // token -> session
    uint32_t web_session_count_ = 0;
    LoginGuard logins_;
    AccountBook signed_in_;
    std::mutex mutex_;

    UserAccount* find_user_by_name_locked(const std::string& username);
    // Whom a nickname and hostmask stand for, without copying the account; null if nobody
    const UserAccount* effective_user_locked(const std::string& network, const std::string& nick, const std::string& hostmask);
    void end_sessions_locked(const std::string& username, const std::string& keep_token = "");
};

extern SecurityManager g_security_mgr;
