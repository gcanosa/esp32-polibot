#include "security_manager.h"
#include "bot_settings.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <strings.h>
#include <iomanip>
#include <sstream>

static const char* TAG = "SECURITY";

int get_role_level(const std::string& role) {
    std::string r = role;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    if (r == "owner") return 100;
    if (r == "admin") return 75;
    if (r == "mod" || r == "moderator") return 50;
    if (r == "user") return 10;
    return 0;
}

// A login on one network says nothing about the same nick on another
static std::string session_key(const std::string& network, const std::string& nick) {
    std::string key = network + " " + nick;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    return key;
}

SecurityManager::SecurityManager(ConfigManager& config_mgr) : config_mgr_(config_mgr) {
    config_mgr_.register_rehash_listener([this](cJSON* root) {
        this->on_rehash(root);
    });
}

void SecurityManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    users_.clear();

    cJSON* root = config_mgr_.get_root();
    if (!root) return;

    cJSON* users_arr = cJSON_GetObjectItemCaseSensitive(root, "users");
    if (cJSON_IsArray(users_arr)) {
        int count = cJSON_GetArraySize(users_arr);
        for (int i = 0; i < count; i++) {
            cJSON* item = cJSON_GetArrayItem(users_arr, i);
            if (!cJSON_IsObject(item)) continue;

            UserAccount ua;
            cJSON* u_item = cJSON_GetObjectItemCaseSensitive(item, "username");
            cJSON* s_item = cJSON_GetObjectItemCaseSensitive(item, "salt");
            cJSON* p_item = cJSON_GetObjectItemCaseSensitive(item, "password_hash");
            cJSON* g_item = cJSON_GetObjectItemCaseSensitive(item, "global_role");

            if (u_item && u_item->valuestring) ua.username = u_item->valuestring;
            if (s_item && s_item->valuestring) ua.salt = s_item->valuestring;
            if (p_item && p_item->valuestring) ua.password_hash = p_item->valuestring;
            ua.global_role = (g_item && g_item->valuestring) ? g_item->valuestring : "user";

            cJSON* ch_roles = cJSON_GetObjectItemCaseSensitive(item, "channel_roles");
            if (cJSON_IsObject(ch_roles)) {
                cJSON* ch = nullptr;
                cJSON_ArrayForEach(ch, ch_roles) {
                    if (ch->string && ch->valuestring) {
                        std::string ch_name = ch->string;
                        std::transform(ch_name.begin(), ch_name.end(), ch_name.begin(), ::tolower);
                        ua.channel_roles[ch_name] = ch->valuestring;
                    }
                }
            }

            cJSON* masks = cJSON_GetObjectItemCaseSensitive(item, "hostmasks");
            if (cJSON_IsArray(masks)) {
                int m_cnt = cJSON_GetArraySize(masks);
                for (int m = 0; m < m_cnt; m++) {
                    cJSON* mi = cJSON_GetArrayItem(masks, m);
                    if (mi && mi->valuestring) ua.hostmasks.push_back(mi->valuestring);
                }
            }
            if (ua.hostmasks.empty()) ua.hostmasks.push_back("*!*@*");

            cJSON* account = nullptr;
            cJSON_ArrayForEach(account, cJSON_GetObjectItemCaseSensitive(item, "accounts")) {
                if (cJSON_IsString(account) && account->valuestring[0]) ua.accounts.push_back(account->valuestring);
            }
            ua.default_password = verify_password(DEFAULT_PASSWORD, ua.salt, ua.password_hash);

            users_.push_back(ua);
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d security user accounts", users_.size());
}

void SecurityManager::on_rehash(cJSON* root) {
    load();
    std::lock_guard<std::mutex> lock(mutex_);
    irc_sessions_.clear();
    ESP_LOGI(TAG, "Security Manager rehashed. Active sessions cleared.");
}

std::string SecurityManager::generate_salt(size_t hex_len) {
    size_t byte_len = hex_len / 2;
    std::vector<uint8_t> rand_bytes(byte_len);
    esp_fill_random(rand_bytes.data(), byte_len);

    std::ostringstream oss;
    for (uint8_t b : rand_bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return oss.str().substr(0, hex_len);
}

std::string SecurityManager::compute_sha256_hex(const std::string& input) {
    uint8_t hash[32];
    mbedtls_sha256((const unsigned char*)input.data(), input.size(), hash, 0);

    std::ostringstream oss;
    for (int i = 0; i < 32; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return oss.str();
}

bool SecurityManager::hash_password(const std::string& password, std::string& salt, std::string& out_hash) {
    if (salt.empty()) {
        salt = generate_salt(8);
    }
    out_hash = compute_sha256_hex(salt + password);
    return true;
}

bool SecurityManager::verify_password(const std::string& password, const std::string& salt, const std::string& expected_hash) {
    std::string computed = compute_sha256_hex(salt + password);
    std::string h1 = computed;
    std::string h2 = expected_hash;
    std::transform(h1.begin(), h1.end(), h1.begin(), ::tolower);
    std::transform(h2.begin(), h2.end(), h2.begin(), ::tolower);
    return (h1 == h2);
}

UserAccount* SecurityManager::find_user_by_name_locked(const std::string& username) {
    std::string u_lower = username;
    std::transform(u_lower.begin(), u_lower.end(), u_lower.begin(), ::tolower);
    for (auto& u : users_) {
        std::string cand = u.username;
        std::transform(cand.begin(), cand.end(), cand.begin(), ::tolower);
        if (cand == u_lower) return &u;
    }
    return nullptr;
}

int SecurityManager::login_blocked_for(const std::string& source) {
    std::lock_guard<std::mutex> lock(mutex_);
    return logins_.blocked_for(irc_lower(source), esp_timer_get_time() / 1000000);
}

bool SecurityManager::login_failed(const std::string& source) {
    const auto settings = g_settings.get();
    std::lock_guard<std::mutex> lock(mutex_);
    return logins_.failed(irc_lower(source), esp_timer_get_time() / 1000000, settings->login_attempts,
                          settings->login_window_minutes * 60, settings->login_block_minutes * 60);
}

std::pair<bool, std::string> SecurityManager::irc_login(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& password, const std::string& username) {
    // failures are counted per host: nicknames and idents are the sender's to choose
    const size_t at = hostmask.rfind('@');
    const std::string source = "irc " + network + " " + (at == std::string::npos ? hostmask : hostmask.substr(at + 1));
    if (const int wait = login_blocked_for(source)) {
        return {false, "Too many failed logins. Try again in " + format_duration(wait) + "."};
    }
    auto refuse = [&](const std::string& why) -> std::pair<bool, std::string> {
        return {false, login_failed(source) ? why + " Too many failures: logins from your host are paused." : why};
    };

    std::unique_lock<std::mutex> lock(mutex_);

    UserAccount* target = nullptr;
    if (!username.empty()) {
        target = find_user_by_name_locked(username);
    } else {
        // Try matching nick with username, or match by hostmask
        target = find_user_by_name_locked(nick);
        if (!target) {
            for (auto& u : users_) {
                for (auto& m : u.hostmasks) {
                    if (wildcard_match(m, hostmask)) {
                        target = &u;
                        break;
                    }
                }
                if (target) break;
            }
        }
    }

    if (!target) {
        lock.unlock();
        return refuse("Login failed: Account not found.");
    }

    if (!verify_password(password, target->salt, target->password_hash)) {
        lock.unlock();
        return refuse("Login failed: Incorrect password.");
    }

    if (target->default_password) {
        return {false, "Login refused: change the default password in the web panel first."};
    }

    // Check hostmask authorized
    bool mask_ok = false;
    for (const auto& m : target->hostmasks) {
        if (wildcard_match(m, hostmask)) {
            mask_ok = true;
            break;
        }
    }
    if (!mask_ok) {
        return {false, "Login failed: Hostmask " + hostmask + " not authorized for " + target->username};
    }

    irc_sessions_[session_key(network, nick)] = {target->username, hostmask};
    std::string msg = "Authenticated successfully as '" + target->username + "' (Global Role: " + target->global_role + ").";
    return {true, msg};
}

std::pair<bool, std::string> SecurityManager::irc_logout(const std::string& network, const std::string& nick) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (irc_sessions_.erase(session_key(network, nick))) {
        return {true, "Logged out successfully."};
    }
    return {false, "You were not logged in."};
}

const UserAccount* SecurityManager::effective_user_locked(const std::string& network, const std::string& nick, const std::string& hostmask) {
    // The web panel has already authenticated this account
    if (hostmask == WEB_CONSOLE_HOSTMASK) return find_user_by_name_locked(nick);

    // A session belongs to whoever logged in, not to whoever holds the nick later
    auto it = irc_sessions_.find(session_key(network, nick));
    if (it != irc_sessions_.end() && it->second.hostmask == hostmask) {
        if (const UserAccount* u = find_user_by_name_locked(it->second.username)) return u;
    }

    // Signed in to a services account that belongs to a user
    const std::string account = signed_in_.find(network, nick, user_host(hostmask));
    if (!account.empty()) {
        const std::string wanted = network + "/" + account;
        for (const auto& u : users_) {
            if (contains_ci(u.accounts, wanted)) return &u;
        }
    }

    // Fallback: trusted hostmask. One that names no host ("*!*@*", "*!alice@*") only allows a password login.
    for (const auto& u : users_) {
        for (const auto& m : u.hostmasks) {
            if (mask_names_host(m) && wildcard_match(m, hostmask)) return &u;
        }
    }
    return nullptr;
}

bool SecurityManager::get_effective_user(const std::string& network, const std::string& nick, const std::string& hostmask, UserAccount& out_user) {
    std::lock_guard<std::mutex> lock(mutex_);
    const UserAccount* u = effective_user_locked(network, nick, hostmask);
    if (u) out_user = *u;
    return u != nullptr;
}

// The account's role there: the global one, or a higher one it holds in that channel
static std::string role_in(const UserAccount& u, const std::string& network, const std::string& channel) {
    std::string role = u.global_role;
    if (!channel.empty()) {
        // channel roles may apply everywhere ("#chan") or on one network ("network/#chan")
        for (std::string key : {channel, network + "/" + channel}) {
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            auto it = u.channel_roles.find(key);
            if (it != u.channel_roles.end() && get_role_level(it->second) > get_role_level(role)) {
                role = it->second;
            }
        }
    }
    return role;
}

Identity SecurityManager::identify(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    Identity who;
    who.account = signed_in_.find(network, nick, user_host(hostmask));
    const UserAccount* u = effective_user_locked(network, nick, hostmask);
    if (u) who.user = u->username;
    who.level = get_role_level(u ? role_in(*u, network, channel) : "user");
    return who;
}

std::string SecurityManager::get_role(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& channel) {
    // asked for every channel message: nothing of the account is copied
    std::lock_guard<std::mutex> lock(mutex_);
    const UserAccount* u = effective_user_locked(network, nick, hostmask);
    return u ? role_in(*u, network, channel) : "user";
}

bool SecurityManager::has_permission(const std::string& network, const std::string& nick, const std::string& hostmask, const std::string& required_role, const std::string& channel) {
    std::string role = get_role(network, nick, hostmask, channel);
    return get_role_level(role) >= get_role_level(required_role);
}

bool SecurityManager::note_account(const std::string& network, const std::string& nick, const std::string& userhost, const std::string& account) {
    std::lock_guard<std::mutex> lock(mutex_);
    return signed_in_.note(network, nick, userhost, account);
}

void SecurityManager::rename_account(const std::string& network, const std::string& from, const std::string& to) {
    std::lock_guard<std::mutex> lock(mutex_);
    signed_in_.rename(network, from, to);
}

void SecurityManager::forget_accounts(const std::string& network) {
    std::lock_guard<std::mutex> lock(mutex_);
    signed_in_.forget_network(network);
}

bool SecurityManager::has_owner_session(const std::string& network, const std::string& nick, const std::string& hostmask) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = irc_sessions_.find(session_key(network, nick));
    if (it == irc_sessions_.end() || it->second.hostmask != hostmask) return false;
    UserAccount* u = find_user_by_name_locked(it->second.username);
    return u && get_role_level(u->global_role) >= get_role_level("owner");
}

bool SecurityManager::authenticate(const std::string& username, const std::string& password, UserAccount& out_user) {
    std::lock_guard<std::mutex> lock(mutex_);
    UserAccount* u = find_user_by_name_locked(username);
    if (!u) return false;
    if (!verify_password(password, u->salt, u->password_hash)) return false;
    out_user = *u;
    return true;
}

std::string SecurityManager::create_web_session(const std::string& username) {
    std::string token = generate_salt(32);
    const int64_t now = esp_timer_get_time();
    const int64_t idle_limit = g_settings.get()->session_minutes * 60000000LL;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = web_sessions_.begin(); it != web_sessions_.end();) {
        if (now - it->second.last_used > idle_limit) it = web_sessions_.erase(it);
        else ++it;
    }
    while (web_sessions_.size() >= MAX_WEB_SESSIONS) {
        web_sessions_.erase(std::min_element(web_sessions_.begin(), web_sessions_.end(),
                                             [](const auto& a, const auto& b) { return a.second.last_used < b.second.last_used; }));
    }
    web_sessions_[token] = {username, now};
    return token;
}

bool SecurityManager::validate_web_session(const std::string& token, UserAccount& out_user, bool counts_as_activity) {
    const int64_t idle_limit = g_settings.get()->session_minutes * 60000000LL;
    const int64_t now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = web_sessions_.find(token);
    if (it == web_sessions_.end()) return false;
    if (now - it->second.last_used > idle_limit) {
        web_sessions_.erase(it);
        return false;
    }
    UserAccount* u = find_user_by_name_locked(it->second.username);
    if (!u) return false;
    if (counts_as_activity) it->second.last_used = now;
    out_user = *u;
    return true;
}

void SecurityManager::destroy_web_session(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    web_sessions_.erase(token);
}

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// "network/account", both usable as one word of an IRC command
static bool is_network_account(const std::string& entry) {
    const size_t slash = entry.find('/');
    if (slash == std::string::npos || slash == 0 || slash > 32) return false;
    const std::string account = entry.substr(slash + 1);
    if (account.empty() || account.size() > 31 || account.find_first_of(" \t\r\n,/") != std::string::npos) return false;
    return std::all_of(entry.begin(), entry.begin() + slash, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

// New names must work as one word of an IRC command and are safe to show in the panel
static bool valid_username(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || std::strchr("_-.[]{}^|`", c); });
}

void SecurityManager::end_sessions_locked(const std::string& username, const std::string& keep_token) {
    for (auto it = web_sessions_.begin(); it != web_sessions_.end();) {
        if (strcasecmp(it->second.username.c_str(), username.c_str()) == 0 && it->first != keep_token) it = web_sessions_.erase(it);
        else ++it;
    }
    for (auto it = irc_sessions_.begin(); it != irc_sessions_.end();) {
        if (strcasecmp(it->second.username.c_str(), username.c_str()) == 0) it = irc_sessions_.erase(it);
        else ++it;
    }
}

std::pair<bool, std::string> SecurityManager::add_or_update_user(const std::string& username, const std::string& password, const std::string& new_role, const std::map<std::string, std::string>* new_channel_roles, const std::vector<std::string>& hostmasks, const std::string& keep_token, const std::vector<std::string>* accounts) {
    // roles and channel names are stored and compared in lower case
    const std::string global_role = lower(new_role);
    std::map<std::string, std::string> channel_roles;
    if (new_channel_roles) {
        for (const auto& r : *new_channel_roles) channel_roles[lower(r.first)] = lower(r.second);
    }
    // IRC "login <password>" splits on spaces
    if (password.size() > 128 || password.find_first_of(" \t\r\n") != std::string::npos) {
        return {false, "Passwords cannot contain spaces (128 characters at most)."};
    }
    if (password == DEFAULT_PASSWORD) return {false, "That is the default password: choose another."};
    if (accounts) {
        for (const auto& a : *accounts) {
            if (!is_network_account(a)) return {false, "Services accounts are written network/account, e.g. SekurNet/alice."};
        }
    }
    if (!global_role.empty() && get_role_level(global_role) == 0) return {false, "Unknown role '" + global_role + "'."};
    for (const auto& r : channel_roles) {
        if (get_role_level(r.second) == 0) return {false, "Unknown role '" + r.second + "'."};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    UserAccount* u = find_user_by_name_locked(username);
    const bool is_new = !u;
    if (u) {
        const int owner = get_role_level("owner");
        if (!global_role.empty() && get_role_level(u->global_role) >= owner && get_role_level(global_role) < owner &&
            std::count_if(users_.begin(), users_.end(), [&](const UserAccount& a) { return get_role_level(a.global_role) >= owner; }) <= 1) {
            return {false, "At least one owner account is required."};
        }
        if (!password.empty()) {
            u->salt = generate_salt(8);
            hash_password(password, u->salt, u->password_hash);
            u->default_password = false;
            end_sessions_locked(u->username, keep_token);
        }
        if (!global_role.empty()) u->global_role = global_role;
        if (new_channel_roles) u->channel_roles = channel_roles; // also when emptied
        if (!hostmasks.empty()) u->hostmasks = hostmasks;
        if (accounts) u->accounts = *accounts;
    } else {
        if (!valid_username(username)) return {false, "Usernames use 1-32 letters, digits or _-.[]{}^|` characters."};
        if (password.empty()) return {false, "A password is required for a new account."};
        UserAccount nu;
        nu.username = username;
        nu.salt = generate_salt(8);
        hash_password(password, nu.salt, nu.password_hash);
        nu.global_role = global_role.empty() ? "user" : global_role;
        nu.channel_roles = channel_roles;
        nu.hostmasks = hostmasks.empty() ? std::vector<std::string>{"*!*@*"} : hostmasks;
        if (accounts) nu.accounts = *accounts;
        users_.push_back(nu);
    }
    if (!persist_users()) return {false, "Could not save to flash."};
    return {true, "User '" + username + (is_new ? "' created." : "' updated.")};
}

std::pair<bool, std::string> SecurityManager::set_password(const std::string& username, const std::string& password, const std::string& keep_token) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!find_user_by_name_locked(username)) return {false, "User not found."};
    }
    if (password.empty()) return {false, "The new password is empty."};
    auto result = add_or_update_user(username, password, "", nullptr, {}, keep_token);
    if (result.first) result.second = "Password changed for '" + username + "'.";
    return result;
}

bool SecurityManager::remove_user(const std::string& username, std::string& out_msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string u_lower = username;
    std::transform(u_lower.begin(), u_lower.end(), u_lower.begin(), ::tolower);

    for (auto it = users_.begin(); it != users_.end(); ++it) {
        std::string cand = it->username;
        std::transform(cand.begin(), cand.end(), cand.begin(), ::tolower);
        if (cand == u_lower) {
            if (get_role_level(it->global_role) >= get_role_level("owner")) {
                out_msg = "Cannot delete owner account.";
                return false;
            }
            end_sessions_locked(it->username);
            users_.erase(it);
            if (!persist_users()) {
                out_msg = "Could not save to flash.";
                return false;
            }
            out_msg = "User '" + username + "' deleted.";
            return true;
        }
    }
    out_msg = "User not found.";
    return false;
}

std::vector<UserAccount> SecurityManager::list_users() {
    std::lock_guard<std::mutex> lock(mutex_);
    return users_;
}

bool SecurityManager::persist_users() {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& u : users_) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "username", u.username.c_str());
        cJSON_AddStringToObject(obj, "salt", u.salt.c_str());
        cJSON_AddStringToObject(obj, "password_hash", u.password_hash.c_str());
        cJSON_AddStringToObject(obj, "global_role", u.global_role.c_str());

        cJSON* cr = cJSON_CreateObject();
        for (const auto& pair : u.channel_roles) {
            cJSON_AddStringToObject(cr, pair.first.c_str(), pair.second.c_str());
        }
        cJSON_AddItemToObject(obj, "channel_roles", cr);

        cJSON* masks = cJSON_CreateArray();
        for (const auto& m : u.hostmasks) {
            cJSON_AddItemToArray(masks, cJSON_CreateString(m.c_str()));
        }
        cJSON_AddItemToObject(obj, "hostmasks", masks);

        if (!u.accounts.empty()) {
            cJSON* accounts = cJSON_AddArrayToObject(obj, "accounts");
            for (const auto& a : u.accounts) cJSON_AddItemToArray(accounts, cJSON_CreateString(a.c_str()));
        }

        cJSON_AddItemToArray(arr, obj);
    }
    return config_mgr_.set_item("users", arr) && config_mgr_.save();
}
