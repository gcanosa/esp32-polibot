// IRCClient: the life of one network connection
#include "irc_client.h"
#include "irc_internal.h"
#include <esp_idf_version.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_app_desc.h>
#include <algorithm>

std::string software_description() {
#if CONFIG_IDF_TARGET_ESP32S3
    const char* chip_name = "ESP32-S3";
#else
    const char* chip_name = "ESP32";
#endif
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    esp_flash_get_size(nullptr, &flash_bytes);
    char buf[192];
    snprintf(buf, sizeof(buf), "PoliBOT v%s - Running on %s (%d cores @ %d MHz, %u MB flash) - ESP-IDF %s",
             esp_app_get_description()->version, chip_name, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             (unsigned)(flash_bytes >> 20), esp_get_idf_version());
    return buf;
}

IRCClient::IRCClient(ConfigManager& config_mgr, SecurityManager& security_mgr, GuardianEngine& guardian, const IrcSettings& settings)
    : config_mgr_(config_mgr), security_mgr_(security_mgr), guardian_(guardian), name_(settings.name),
      cfg_(std::make_shared<IrcSettings>(settings)) {}

IRCClient::~IRCClient() {
    close_connection();
}

void IRCClient::log(const std::string& msg) {
    g_log_buffer.log("[" + name_ + "] " + msg);
}

std::string IRCClient::current_nick() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_nick_.empty() ? cfg_->nick : current_nick_;
}

std::shared_ptr<const IrcSettings> IRCClient::settings() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return cfg_;
}

bool IRCClient::start() {
    start_time_ = seconds_now();
    // the task owns a reference, so the object outlives removal from the manager
    auto* self = new std::shared_ptr<IRCClient>(shared_from_this());
    if (xTaskCreatePinnedToCore(task_entry, "irc_task", 12288, self, 5, nullptr, 0) != pdPASS) {
        delete self;
        log("Not enough memory to start this network.");
        return false;
    }
    return true;
}

void IRCClient::stop() {
    stop_ = true;
}

void IRCClient::task_entry(void* pv) {
    // lines keep being read while settings changed here are written
    ConfigManager::defer_writes_on_this_task();
    {
        std::unique_ptr<std::shared_ptr<IRCClient>> self(static_cast<std::shared_ptr<IRCClient>*>(pv));
        (*self)->run_loop();
    } // release before vTaskDelete(), which never returns
    vTaskDelete(nullptr);
}

void IRCClient::apply(const IrcSettings& next) {
    IrcSettings prev;
    std::vector<std::string> joined;
    bool moved, rekeyed;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        prev = *cfg_;
        cfg_ = std::make_shared<IrcSettings>(next);
        for (const auto& c : channels_) joined.push_back(c.second.name);
        moved = !same(prev.report_channel, next.report_channel);
        rekeyed = prev.report_key != next.report_key;
        // nothing waits to be joined that is no longer wanted
        for (const auto& ch : prev.channels) {
            if (!contains_ci(next.channels, ch)) pending_joins_.erase(irc_lower(ch));
        }
        if (moved) {
            pending_joins_.erase(irc_lower(prev.report_channel));
            report_greeted_ = false;
        }
        if (moved || rekeyed) {
            report_keyed_ = false;
            report_key_set_at_ = -60;
            report_problem_.clear();
        }
    }
    if (!next.same_connection(prev)) {
        log("Connection settings changed. Reconnecting...");
        reconnect();
        return;
    }
    const bool renamed = next.nick != prev.nick;
    if (!connected_) { // used at the next attempt; channels are joined after the welcome
        if (renamed) retry_now();
        return;
    }
    if (renamed) {
        log("Changing nickname to " + next.nick + "...");
        send_raw("NICK " + next.nick);
    }
    if (registered_ && next.bot_mode != prev.bot_mode) apply_bot_mode(!next.bot_mode);
    for (const auto& ch : next.channels) {
        if (!contains_ci(joined, ch)) send_raw(join_line(ch));
    }
    for (const auto& ch : prev.channels) {
        if (!contains_ci(next.channels, ch) && !same(ch, next.report_channel)) send_raw("PART " + ch + " :Removed");
    }
    if (moved && !prev.report_channel.empty() && !contains_ci(next.channels, prev.report_channel)) {
        send_raw("PART " + prev.report_channel + " :No longer the report channel");
    }
    if (registered_ && (moved || rekeyed) && !next.report_channel.empty()) {
        if (!contains_ci(joined, next.report_channel)) {
            send_raw(join_line(next.report_channel));
        } else {
            if (rekeyed && !moved && !prev.report_key.empty() && can_act(next.report_channel)) {
                send_raw("MODE " + next.report_channel + " -k " + prev.report_key);
            }
            send_raw("MODE " + next.report_channel); // the reply sets the new password if needed
        }
    }
    for (const auto& ch : get_joined_channels()) sweep_akicks(ch); // the auto-kick list may have grown
    review_modes(); // and so may the auto-op and voice lists, or their rules
}

void IRCClient::run_loop() {
    int delay = settings()->reconnect_delay;
    while (!stop_) {
        reconnect_ = false; // changes made from here on trigger another reconnect
        wake_ = false;
        if (connect_once()) {
            delay = settings()->reconnect_delay;
            read_loop();
            if (stop_) send_raw("QUIT :Network removed");
            else if (reconnect_) send_raw("QUIT :Reconnecting");
        }
        close_connection();
        if (stop_) break;

        if (reconnect_) {
            delay = settings()->reconnect_delay;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        log("Disconnected. Reconnecting in " + std::to_string(delay) + "s...");
        for (int i = 0; i < delay * 10 && !stop_ && !reconnect_ && !wake_; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        delay = wake_ ? settings()->reconnect_delay : std::min(delay * 2, settings()->max_reconnect_delay);
    }
    log("Network stopped.");
}

void IRCClient::pause_ms(int ms) {
    for (int i = 0; i < ms / 100 && !stop_ && !reconnect_; i++) vTaskDelay(pdMS_TO_TICKS(100));
}

std::vector<std::string> IRCClient::get_joined_channels() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::vector<std::string> joined;
    for (const auto& c : channels_) {
        if (!same(c.second.name, cfg_->report_channel)) joined.push_back(c.second.name);
    }
    return joined;
}

std::vector<ChannelDetail> IRCClient::get_channels_detail() {
    std::vector<ChannelDetail> list;
    for (const auto& ch : get_joined_channels()) {
        ChannelGuardianConfig cfg = guardian_.get_channel_config(name_, ch);

        ChannelDetail d;
        d.network = name_;
        d.name = ch;
        d.users = 0;
        d.active_limit = 0;
        d.op = -1;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channels_.find(irc_lower(ch));
            if (it != channels_.end()) {
                d.users = it->second.members.size();
                d.active_limit = it->second.limit.current;
                d.op = it->second.op;
            }
        }
        d.dyn_enabled = cfg.dynamic_limit.enabled;
        d.offset = cfg.dynamic_limit.offset;
        d.entry_enabled = cfg.entrymsg.enabled;
        d.entry_msg = cfg.entrymsg.message;
        d.lockdown = cfg.lockdown;
        list.push_back(d);
    }
    return list;
}
