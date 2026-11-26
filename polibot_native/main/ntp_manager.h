#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cJSON.h>
#include "config_manager.h"

struct TimezonePreset {
    const char* label;      // e.g. "Europe/Madrid (CET/CEST)"
    const char* location;   // e.g. "Europe/Madrid"
    const char* posix_tz;   // e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
};

class NtpManager {
public:
    explicit NtpManager(ConfigManager& config_mgr);
    ~NtpManager();

    bool init();
    void start();
    void stop();
    void on_rehash(cJSON* root);

    bool is_synced() const;
    std::string get_server() const;
    std::string get_timezone() const;
    std::string get_location() const;
    int64_t get_epoch_seconds() const;

    // Returns formatted local time string, e.g. "2026-11-25 01:45:00 CET"
    std::string get_formatted_time(const std::string& fmt = "%Y-%m-%d %H:%M:%S %Z");

    // Dynamic config updates
    void update_config(bool enabled, const std::string& server, const std::string& tz_str, const std::string& location);

    static const std::vector<TimezonePreset>& get_presets();

private:
    ConfigManager& config_mgr_;
    bool enabled_ = true;
    std::string server_ = "pool.ntp.org";
    std::string timezone_ = "CET-1CEST,M3.5.0,M10.5.0/3";
    std::string location_ = "Europe/Madrid";
    bool started_ = false;
    mutable std::mutex mutex_;

    void apply_timezone_locked();
    void load_config_locked();
    void start_locked();
};

extern NtpManager g_ntp_mgr;
