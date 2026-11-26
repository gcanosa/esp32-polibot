#include "ntp_manager.h"
#include <esp_log.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>
#include <cstring>

static const char* TAG = "NTP";

static const std::vector<TimezonePreset> PRESETS = {
    {"Europe/Madrid, Paris, Rome, Berlin (CET/CEST)", "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London, Dublin, Lisbon (GMT/BST)", "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Athens, Helsinki, Bucharest (EET/EEST)", "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"America/New_York, Miami, Toronto (EST/EDT)", "America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago, Dallas, Mexico City (CST/CDT)", "America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver, Phoenix, Calgary (MST/MDT)", "America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles, Vancouver (PST/PDT)", "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Sao_Paulo, Buenos Aires (BRT)", "America/Sao_Paulo", "BRT3"},
    {"Asia/Dubai (GST)", "Asia/Dubai", "GST-4"},
    {"Asia/Kolkata, New Delhi (IST)", "Asia/Kolkata", "IST-5:30"},
    {"Asia/Shanghai, Hong Kong, Singapore (CST)", "Asia/Shanghai", "CST-8"},
    {"Asia/Tokyo, Seoul (JST/KST)", "Asia/Tokyo", "JST-9"},
    {"Australia/Sydney, Melbourne (AEST/AEDT)", "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Universal Coordinated Time (UTC)", "UTC", "UTC0"}
};

const std::vector<TimezonePreset>& NtpManager::get_presets() {
    return PRESETS;
}

static void time_sync_notification_cb(struct timeval* tv) {
    ESP_LOGI(TAG, "SNTP time synchronized with server!");
}

NtpManager::NtpManager(ConfigManager& config_mgr) : config_mgr_(config_mgr) {
    config_mgr_.register_rehash_listener([this](cJSON* root) {
        this->on_rehash(root);
    });
}

NtpManager::~NtpManager() {
    stop();
}

void NtpManager::load_config_locked() {
    cJSON* root = config_mgr_.get_root();
    if (!root) return;

    cJSON* ntp = cJSON_GetObjectItemCaseSensitive(root, "ntp");
    if (cJSON_IsObject(ntp)) {
        cJSON* en = cJSON_GetObjectItemCaseSensitive(ntp, "enabled");
        cJSON* srv = cJSON_GetObjectItemCaseSensitive(ntp, "server");
        cJSON* tz = cJSON_GetObjectItemCaseSensitive(ntp, "timezone");
        cJSON* loc = cJSON_GetObjectItemCaseSensitive(ntp, "location");

        enabled_ = en ? cJSON_IsTrue(en) : true;
        if (srv && srv->valuestring) server_ = srv->valuestring;
        if (tz && tz->valuestring) timezone_ = tz->valuestring;
        if (loc && loc->valuestring) location_ = loc->valuestring;
    }
    cJSON_Delete(root);
}

void NtpManager::apply_timezone_locked() {
    setenv("TZ", timezone_.c_str(), 1);
    tzset();
    ESP_LOGI(TAG, "Applied Timezone: '%s' (Location: %s)", timezone_.c_str(), location_.c_str());
}

bool NtpManager::init() {
    std::lock_guard<std::mutex> lock(mutex_);
    load_config_locked();
    apply_timezone_locked();
    return true;
}

void NtpManager::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    start_locked();
}

void NtpManager::start_locked() {
    if (!enabled_ || started_) return;

    ESP_LOGI(TAG, "Initializing SNTP client with server: %s", server_.c_str());
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server_.c_str());
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    started_ = true;
}

void NtpManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        esp_sntp_stop();
        started_ = false;
        ESP_LOGI(TAG, "SNTP stopped.");
    }
}

void NtpManager::on_rehash(cJSON* root) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string old_server = server_;
    std::string old_tz = timezone_;

    load_config_locked();

    if (timezone_ != old_tz) {
        apply_timezone_locked();
    }

    if (!enabled_ && started_) {
        esp_sntp_stop();
        started_ = false;
    } else if (enabled_ && !started_) {
        start_locked();
    } else if (enabled_ && started_ && server_ != old_server) {
        esp_sntp_stop();
        esp_sntp_setservername(0, server_.c_str());
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP server updated to: %s", server_.c_str());
    }
}

bool NtpManager::is_synced() const {
    sntp_sync_status_t status = sntp_get_sync_status();
    return (status == SNTP_SYNC_STATUS_COMPLETED);
}

std::string NtpManager::get_server() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return server_;
}

std::string NtpManager::get_timezone() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timezone_;
}

std::string NtpManager::get_location() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return location_;
}

int64_t NtpManager::get_epoch_seconds() const {
    time_t now;
    time(&now);
    return (int64_t)now;
}

std::string NtpManager::get_formatted_time(const std::string& fmt) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char buf[128];
    strftime(buf, sizeof(buf), fmt.c_str(), &timeinfo);
    return std::string(buf);
}

void NtpManager::update_config(bool enabled, const std::string& server, const std::string& tz_str, const std::string& location) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = enabled;
        server_ = server;
        timezone_ = tz_str;
        location_ = location;
        apply_timezone_locked();
    }

    cJSON* root = config_mgr_.get_root();
    if (!root) return;

    cJSON* ntp = cJSON_CreateObject();
    cJSON_AddBoolToObject(ntp, "enabled", enabled);
    cJSON_AddStringToObject(ntp, "server", server.c_str());
    cJSON_AddStringToObject(ntp, "timezone", tz_str.c_str());
    cJSON_AddStringToObject(ntp, "location", location.c_str());

    config_mgr_.set_item("ntp", ntp);
    config_mgr_.save();
    ESP_LOGI(TAG, "NTP config persisted to flash.");
}
