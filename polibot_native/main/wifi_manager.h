#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <esp_err.h>
#include <esp_event.h>
#include "config_manager.h"
#include "wifi_policy.h"

class WiFiManager {
public:
    explicit WiFiManager(ConfigManager& config_mgr);

    // Brings up the radio and a supervisor task that reconnects, opens the setup AP and applies new settings
    bool start();
    // Called from the supervisor task each time the station gets an address
    void on_online(std::function<void(bool ip_changed)> callback) { online_cb_ = callback; }

    bool is_connected() const { return connected_; } // joined the configured network
    bool ap_active() const { return ap_on_; }
    bool reachable() const { return connected_ || ap_on_; }
    std::string get_ip(); // station address, else the setup AP's

private:
    struct Settings {
        std::string ssid, password, ap_ssid, ap_password;
        bool ap_fallback = true;
    };

    ConfigManager& config_mgr_;
    std::function<void(bool)> online_cb_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ap_on_{false};
    std::atomic<bool> announce_{false};
    std::atomic<bool> ip_changed_{false};
    std::atomic<bool> reload_{false};
    std::mutex mutex_;      // guards ip_
    std::string ip_;        // last station address
    int last_reason_ = 0;   // event task only
    Settings cfg_;          // start() and the supervisor task only
    WifiPolicy policy_;

    static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void task_entry(void* pv);
    void supervise();
    void reload();
    Settings load_settings();
    void apply_mode();
    void apply_station();
    void apply_access_point();
};

extern WiFiManager g_wifi_mgr;
