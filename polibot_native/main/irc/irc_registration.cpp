// IRCClient: signing in to services and finding a free nickname
#include "irc_client.h"
#include "irc_internal.h"
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <sstream>
#include <strings.h>


// How the server can tell who is signed in to services
static const char* const ACCOUNT_CAPABILITIES[] = {"account-notify", "extended-join", "account-tag"};

void IRCClient::end_negotiation(const std::string& why) {
    if (!cap_deadline_) return;
    cap_deadline_ = 0;
    cap_requests_ = 0;
    if (!why.empty()) log(why);
    send_raw("CAP END"); // lets registration finish
}

void IRCClient::close_request(const std::string& why) {
    if (!why.empty()) log(why);
    if (cap_deadline_ && --cap_requests_ <= 0) end_negotiation("");
}

// Asks for what the server offers and the bot can use. Accounts and SASL are asked separately, so
// that a refusal of one leaves the other.
void IRCClient::request_capabilities() {
    std::string accounts;
    bool sasl = false;
    std::istringstream offered(server_caps_);
    std::string capability;
    while (offered >> capability) {
        const std::string name = capability.substr(0, capability.find('='));
        for (const char* wanted : ACCOUNT_CAPABILITIES) {
            if (name == wanted) accounts += (accounts.empty() ? "" : " ") + name;
        }
        // "sasl=PLAIN,EXTERNAL" names the methods; a bare "sasl" does not say
        if (name == "sasl" && (capability.size() <= 4 || ("," + capability.substr(5) + ",").find(",PLAIN,") != std::string::npos)) sasl = true;
    }
    const bool signing_in = !settings()->nickserv_pw.empty();
    if (signing_in && !sasl) log("The server offers no usable SASL. Will identify with NickServ.");
    if (!accounts.empty()) {
        cap_requests_++;
        send_raw("CAP REQ :" + accounts);
    }
    if (signing_in && sasl) {
        cap_requests_++;
        send_raw("CAP REQ :sasl");
    }
    if (!cap_requests_) end_negotiation("");
}

// SASL PLAIN: base64 of "account NUL account NUL password", sent in pieces of 400 characters
void IRCClient::send_sasl_credentials() {
    const auto shared = settings();
    const IrcSettings& cfg = *shared;
    const std::string account = cfg.account.empty() ? cfg.nick : cfg.account;
    std::string plain = account;
    plain += '\0';
    plain += account;
    plain += '\0';
    plain += cfg.nickserv_pw;
    std::vector<unsigned char> encoded(plain.size() * 4 / 3 + 8);
    size_t length = 0;
    if (mbedtls_base64_encode(encoded.data(), encoded.size(), &length, reinterpret_cast<const unsigned char*>(plain.data()), plain.size()) != 0) {
        close_request("SASL: could not encode the credentials.");
        return;
    }
    const std::string text(reinterpret_cast<char*>(encoded.data()), length);
    for (size_t at = 0; at < text.size(); at += 400) send_raw("AUTHENTICATE " + text.substr(at, 400));
    if (text.size() % 400 == 0) send_raw("AUTHENTICATE +");
}

// Capability negotiation and SASL replies. True when the line was one of them.
bool IRCClient::capability_command(const std::string& command, const std::vector<std::string>& args, const std::string& trailing) {
    if (command == "CAP") {
        if (!cap_deadline_ || args.size() < 2) return true;
        const std::string sub = irc_lower(args[1]);
        std::string list = trailing;
        for (size_t i = 2; i < args.size(); i++) {
            if (args[i] != "*") list += " " + args[i]; // a single entry may come without the colon
        }
        const std::string words = " " + irc_lower(list) + " ";
        auto names = [&](const char* name) { return words.find(std::string(" ") + name + " ") != std::string::npos; };
        if (sub == "ls") {
            server_caps_ += " " + list;
            if (args.size() > 2 && args[2] == "*") return true; // more lines follow
            request_capabilities();
        } else if (sub == "ack") {
            if (names("sasl")) {
                send_raw("AUTHENTICATE PLAIN"); // the request stays open until SASL is over
            } else {
                if (names("account-notify")) cap_account_notify_ = true;
                if (names("account-tag")) cap_account_tag_ = true;
                if (names("extended-join")) cap_extended_join_ = true;
                close_request("");
            }
        } else if (sub == "nak") {
            close_request(names("sasl") ? "The server refused SASL. Will identify with NickServ." : "");
        }
        return true;
    }
    if (command == "AUTHENTICATE") {
        if (cap_deadline_ && ((!args.empty() && args[0] == "+") || trailing == "+")) send_sasl_credentials();
        return true;
    }
    if (command == "900") return true; // logged in; 903 follows
    if (command == "903" || command == "907") {
        sasl_ok_ = true;
        close_request("Signed in with SASL.");
        return true;
    }
    if (command == "902" || command == "904" || command == "905" || command == "906") {
        close_request("SASL failed (" + trailing + "). Will identify with NickServ.");
        return true;
    }
    return command == "908"; // list of methods; the failure follows
}

constexpr int MAX_NICK_ATTEMPTS = 8;

constexpr size_t NUMBERED_NICK_BASE = 13; // plus three digits stays within the common 16 character limit

// During registration: the alternative nickname first, then numbered ones, so a taken name costs no waiting
void IRCClient::try_another_nick(const IrcSettings& cfg, const std::string& refused) {
    if (nick_attempts_ >= MAX_NICK_ATTEMPTS) {
        log("No usable nickname found. Waiting for the server to close the connection.");
        return;
    }
    std::string next = cfg.alt_nick;
    if (nick_attempts_++ > 0 || next.empty() || strcasecmp(next.c_str(), refused.c_str()) == 0) {
        const std::string& base = cfg.alt_nick.empty() ? cfg.nick : cfg.alt_nick;
        next = base.substr(0, NUMBERED_NICK_BASE) + std::to_string(1 + esp_random() % 999);
    }
    log("Cannot use nickname '" + refused + "'. Trying '" + next + "'...");
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_nick_ = next;
    }
    send_raw("NICK " + next);
}

// One line each, in order, before the channels are joined; the send queue paces them
void IRCClient::run_perform(const IrcSettings& cfg) {
    const std::string nick = current_nick();
    int sent = 0;
    for (size_t at = 0; at < cfg.perform.size();) {
        size_t end = cfg.perform.find('\n', at);
        if (end == PsramString::npos) end = cfg.perform.size();
        const std::string line = client_command(std::string(cfg.perform.data() + at, end - at), nick);
        if (!line.empty() && send_raw(line, true)) sent++;
        at = end + 1;
    }
    // the lines are not logged: they may hold passwords
    if (sent) log("Sent " + std::to_string(sent) + " command(s) set for after connecting.");
}
