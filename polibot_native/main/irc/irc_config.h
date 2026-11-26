#pragma once

// One network as configured under irc.networks
#include <map>
#include <string>
#include <vector>
#include <cJSON.h>
#include "psram_alloc.h"

// ponytail: fixed ceiling set by sockets (CONFIG_LWIP_MAX_SOCKETS=16, shared with the web panel) and RAM
constexpr size_t MAX_IRC_NETWORKS = 4;
constexpr size_t MAX_CHANNEL_KEY = 23; // the shortest limit common servers have
constexpr size_t MAX_JOIN_KEY = 50;    // for channels others manage, whose server may allow longer ones
constexpr int MAX_PERFORM_LINES = 20;  // commands sent after connecting
constexpr size_t MAX_PERFORM_LINE = 400;
constexpr size_t MAX_PERFORM_TOTAL = 2000; // they share the send queue with the joins

struct IrcSettings {
    // A change to any of these reconnects the network
    std::string name;
    std::string host;
    int port = 6667;
    bool ssl = false;
    bool ssl_verify = true;
    std::string nick = "PoliBOT";
    std::string ident = "polibot";
    std::string realname = "ESP32-S3 Guardian Sentry";
    std::string account;     // services account for SASL and NickServ; empty = the nickname
    std::string nickserv_pw; // its password
    // Applied live
    std::string alt_nick;    // tried when the nickname is taken; empty = go straight to numbered ones
    std::vector<std::string> channels;
    std::map<std::string, std::string> channel_keys; // passwords of keyed (+k) channels, by lower case name
    PsramString perform; // commands sent after the welcome, one per line; used at the next connection
    std::string cmd_prefix = "!";
    int reconnect_delay = 5;
    int max_reconnect_delay = 60;
    int ping_timeout = 120;
    bool bot_mode = false;   // ask the server to mark the connection as a bot, if it can
    // Where the bot tells the staff what it does, protected by a password (the channel key)
    std::string report_channel;
    std::string report_key;
    // The server's dialect and services. {channel} and {mask} are filled in; empty = never
    std::string quiet_mode = "+q {mask}";
    std::string ops_command = "PRIVMSG ChanServ :OP {channel}";
    std::string invite_command = "PRIVMSG ChanServ :INVITE {channel}";
    std::string unban_command = "PRIVMSG ChanServ :UNBAN {channel}";

    bool same_connection(const IrcSettings& o) const;
};

// Empty string if the "irc" config section is acceptable, otherwise the reason it is not
std::string validate_irc_config(const cJSON* irc);

// The usable entries of irc.networks
std::vector<IrcSettings> parse_networks(const cJSON* root);
