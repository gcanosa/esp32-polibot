// Reading and checking the "irc" section of the configuration
#include "irc_config.h"
#include "irc_masks.h"
#include "irc_text.h"
#include "log_buffer.h"
#include <algorithm>
#include <cstring>
#include <strings.h>

static std::string json_string(const cJSON* obj, const char* key, const std::string& fallback = "") {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : fallback;
}

static int json_int(const cJSON* obj, const char* key, int fallback) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

static bool json_bool(const cJSON* obj, const char* key, bool fallback) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

// Usable after the channel name in a JOIN
static bool valid_channel_key(const std::string& key, size_t longest) {
    return !key.empty() && key.size() <= longest && key[0] != ':' && key.find_first_of(" ,\t\r\n") == std::string::npos;
}

// One command sent after connecting
static bool perform_line(const cJSON* line) {
    return cJSON_IsString(line) && strlen(line->valuestring) <= MAX_PERFORM_LINE && !strpbrk(line->valuestring, "\r\n");
}

// Names key the channel rules ("name/#channel") and sessions, so they stay simple
static bool valid_network_name(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
}

std::string validate_irc_config(const cJSON* irc) {
    cJSON* nets = cJSON_GetObjectItemCaseSensitive(irc, "networks");
    if (!nets) return ""; // single-network form, converted on load
    if (!cJSON_IsArray(nets)) return "irc.networks must be a list.";
    if ((size_t)cJSON_GetArraySize(nets) > MAX_IRC_NETWORKS) {
        return "At most " + std::to_string(MAX_IRC_NETWORKS) + " networks are supported.";
    }
    std::vector<std::string> names;
    cJSON* n = nullptr;
    cJSON_ArrayForEach(n, nets) {
        std::string name = json_string(n, "name");
        if (!valid_network_name(name)) return "Network names use 1-32 letters, digits, '.', '-' or '_'.";
        if (contains_ci(names, name)) return "Network name '" + name + "' is used twice.";
        names.push_back(name);
        if (trim(json_string(n, "host")).empty()) return "Network '" + name + "' has no server host.";
        int port = json_int(n, "port", 6667);
        if (port < 1 || port > 65535) return "Network '" + name + "' has an invalid port.";
        for (const char* key : {"nick", "alt_nick", "account"}) {
            if (trim(json_string(n, key)).find_first_of(" \t\r\n") != std::string::npos) return "Network '" + name + "': nicknames and the account name cannot contain spaces.";
        }
        const std::string reports = trim(json_string(n, "report_channel"));
        if (!reports.empty()) {
            if (!is_channel_name(reports)) return "Network '" + name + "': the report channel must be a channel name such as #bot-reports.";
            cJSON* ch = nullptr;
            cJSON_ArrayForEach(ch, cJSON_GetObjectItemCaseSensitive(n, "channels")) {
                if (cJSON_IsString(ch) && strcasecmp(ch->valuestring, reports.c_str()) == 0) {
                    return "Network '" + name + "': the report channel cannot also be one of the guarded channels.";
                }
            }
            const std::string key = json_string(n, "report_key");
            if (!valid_channel_key(key, MAX_CHANNEL_KEY)) {
                return "Network '" + name + "': the report channel needs a password of 1-" + std::to_string(MAX_CHANNEL_KEY) + " characters without spaces or commas.";
            }
        }
        cJSON* keys = cJSON_GetObjectItemCaseSensitive(n, "channel_keys");
        if (keys && !cJSON_IsObject(keys)) return "Network '" + name + "': channel_keys must name channels and their passwords.";
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, keys) {
            const std::string key = cJSON_IsString(entry) ? entry->valuestring : "?";
            if (!is_channel_name(entry->string) || (!(cJSON_IsString(entry) && key.empty()) && !valid_channel_key(key, MAX_JOIN_KEY))) {
                return "Network '" + name + "': a channel password has at most " + std::to_string(MAX_JOIN_KEY) + " characters, without spaces or commas.";
            }
        }
        cJSON* perform = cJSON_GetObjectItemCaseSensitive(n, "perform");
        bool usable = !perform || (cJSON_IsArray(perform) && cJSON_GetArraySize(perform) <= MAX_PERFORM_LINES);
        size_t total = 0;
        cJSON* line = nullptr;
        cJSON_ArrayForEach(line, perform) {
            usable = usable && perform_line(line);
            if (usable) total += strlen(line->valuestring);
        }
        if (!usable || total > MAX_PERFORM_TOTAL) {
            return "Network '" + name + "': at most " + std::to_string(MAX_PERFORM_LINES) + " commands after connecting, " +
                   std::to_string(MAX_PERFORM_LINE) + " characters each and " + std::to_string(MAX_PERFORM_TOTAL) + " together.";
        }
        const std::string quiet = json_string(n, "quiet_mode", "+q {mask}");
        if (!quiet.empty() && (quiet.size() < 3 || quiet[0] != '+' || quiet.find("{mask}") == std::string::npos)) {
            return "Network '" + name + "': the mute mode starts with '+' and contains {mask}, e.g. +q {mask}.";
        }
    }
    return "";
}

std::vector<IrcSettings> parse_networks(const cJSON* root) {
    std::vector<IrcSettings> out;
    cJSON* irc = cJSON_GetObjectItemCaseSensitive(root, "irc");
    cJSON* nets = cJSON_GetObjectItemCaseSensitive(irc, "networks");
    std::vector<std::string> names;
    cJSON* n = nullptr;
    cJSON_ArrayForEach(n, nets) {
        IrcSettings s;
        s.name = json_string(n, "name");
        s.host = trim(json_string(n, "host"));
        // a hand-edited config.json can hold anything: skip what cannot work
        if (!valid_network_name(s.name) || s.host.empty() || contains_ci(names, s.name) || out.size() >= MAX_IRC_NETWORKS) {
            g_log_buffer.log("Ignoring invalid or surplus network entry '" + s.name + "'.");
            continue;
        }
        names.push_back(s.name);
        s.port = json_int(n, "port", s.port);
        s.ssl = json_bool(n, "ssl", s.ssl);
        s.ssl_verify = json_bool(n, "ssl_verify", s.ssl_verify);
        s.nick = json_string(n, "nick", s.nick);
        s.ident = json_string(n, "ident", s.ident);
        s.realname = json_string(n, "realname", s.realname);
        s.alt_nick = trim(json_string(n, "alt_nick"));
        s.account = trim(json_string(n, "account"));
        s.nickserv_pw = json_string(n, "nickserv_password");
        s.bot_mode = json_bool(n, "bot_mode", s.bot_mode);
        s.report_channel = trim(json_string(n, "report_channel"));
        s.report_key = json_string(n, "report_key");
        // without a usable password the channel would be open to anyone
        if (!is_channel_name(s.report_channel) || !valid_channel_key(s.report_key, MAX_JOIN_KEY)) s.report_channel.clear();
        cJSON* key = nullptr;
        cJSON_ArrayForEach(key, cJSON_GetObjectItemCaseSensitive(n, "channel_keys")) {
            if (cJSON_IsString(key) && valid_channel_key(key->valuestring, MAX_JOIN_KEY)) s.channel_keys[irc_lower(key->string)] = key->valuestring;
        }
        s.quiet_mode = json_string(n, "quiet_mode", s.quiet_mode);
        if (!s.quiet_mode.empty() && (s.quiet_mode[0] != '+' || s.quiet_mode.find("{mask}") == std::string::npos)) s.quiet_mode.clear();
        s.ops_command = json_string(n, "ops_command", s.ops_command);
        s.invite_command = json_string(n, "invite_command", s.invite_command);
        s.unban_command = json_string(n, "unban_command", s.unban_command);
        cJSON* ch = nullptr;
        cJSON_ArrayForEach(ch, cJSON_GetObjectItemCaseSensitive(n, "channels")) {
            if (cJSON_IsString(ch) && ch->valuestring[0]) s.channels.push_back(ch->valuestring);
        }
        cJSON* line = nullptr;
        int lines = 0;
        cJSON_ArrayForEach(line, cJSON_GetObjectItemCaseSensitive(n, "perform")) {
            if (!perform_line(line) || lines++ >= MAX_PERFORM_LINES ||
                s.perform.size() + strlen(line->valuestring) > MAX_PERFORM_TOTAL + MAX_PERFORM_LINES) continue;
            s.perform += line->valuestring;
            s.perform += '\n';
        }
        s.cmd_prefix = json_string(irc, "cmd_prefix", s.cmd_prefix);
        if (s.cmd_prefix.empty()) s.cmd_prefix = "!";
        s.reconnect_delay = std::max(1, json_int(irc, "reconnect_delay", s.reconnect_delay));
        s.max_reconnect_delay = std::max(s.reconnect_delay, json_int(irc, "max_reconnect_delay", s.max_reconnect_delay));
        s.ping_timeout = std::max(20, json_int(irc, "ping_timeout", s.ping_timeout));
        out.push_back(s);
    }
    return out;
}

bool IrcSettings::same_connection(const IrcSettings& o) const {
    // the nickname is not part of it: IRC can change that on a live connection
    return host == o.host && port == o.port && ssl == o.ssl && ssl_verify == o.ssl_verify &&
           ident == o.ident && realname == o.realname && account == o.account && nickserv_pw == o.nickserv_pw;
}
