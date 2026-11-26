// IRCManager: one client per configured network
#include "irc_client.h"
#include "irc_config.h"
#include <esp_timer.h>
#include <algorithm>
#include <strings.h>

IRCManager::IRCManager(ConfigManager& config_mgr, SecurityManager& security_mgr, GuardianEngine& guardian)
    : config_mgr_(config_mgr), security_mgr_(security_mgr), guardian_(guardian) {
    config_mgr_.register_rehash_listener([this](cJSON*) {
        if (started_) apply_config();
    });
}

void IRCManager::start() {
    start_time_ = esp_timer_get_time() / 1000000;
    started_ = true;
    apply_config();
}

void IRCManager::apply_config() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = config_mgr_.get_root();
    if (!root) return;
    std::vector<IrcSettings> wanted = parse_networks(root);
    cJSON_Delete(root);

    std::vector<std::shared_ptr<IRCClient>> next;
    std::vector<IrcSettings> added;
    for (const auto& s : wanted) {
        auto it = std::find_if(clients_.begin(), clients_.end(), [&](const auto& c) { return c->name() == s.name; });
        // A live connection is changed in place (it says QUIT first, freeing the nickname). One that is still
        // connecting or waiting to retry is replaced, so new server settings are tried at once.
        if (it != clients_.end() && ((*it)->is_connected() || s.same_connection(*(*it)->settings()))) {
            (*it)->apply(s);
            next.push_back(*it);
        } else {
            added.push_back(s);
        }
    }
    // stop removed networks before starting new ones
    for (auto& c : clients_) {
        if (std::find(next.begin(), next.end(), c) == next.end()) c->stop();
    }
    for (const auto& s : added) {
        auto client = std::make_shared<IRCClient>(config_mgr_, security_mgr_, guardian_, s);
        if (client->start()) next.push_back(client);
    }
    clients_.swap(next);
    g_log_buffer.log("IRC settings applied: " + std::to_string(clients_.size()) + " network(s).");
}

void IRCManager::network_up(bool ip_changed) {
    for (auto& c : clients()) {
        if (!c->is_connected()) c->retry_now();
        else if (ip_changed) c->reconnect(); // the old connection died with the old address
    }
}

void IRCManager::report(ReportKind kind, const std::string& text) {
    for (auto& c : clients()) c->report(kind, text);
}

void IRCManager::reconnect_all() {
    for (auto& c : clients()) c->reconnect();
}

std::vector<std::shared_ptr<IRCClient>> IRCManager::clients() {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_;
}

std::shared_ptr<IRCClient> IRCManager::find(const std::string& network) {
    for (auto& c : clients()) {
        if (network.empty() || strcasecmp(c->name().c_str(), network.c_str()) == 0) return c;
    }
    return nullptr;
}
