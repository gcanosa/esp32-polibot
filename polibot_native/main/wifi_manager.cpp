#include "wifi_manager.h"
#include "log_buffer.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>

static const char* TAG = "WIFI";
static const char* AP_ADDRESS = "192.168.4.1";
static const char* DEFAULT_AP_SSID = "PoliBOT-Setup";
static const char* DEFAULT_AP_PASSWORD = "polibotpassword";

WiFiManager::WiFiManager(ConfigManager& config_mgr) : config_mgr_(config_mgr) {
    config_mgr_.register_rehash_listener([this](cJSON*) { reload_ = true; });
}

static const char* reason_hint(int reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return " - network not found (this chip is 2.4 GHz only)";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return " - check the password";
        default: return "";
    }
}

void WiFiManager::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    WiFiManager* self = static_cast<WiFiManager*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const int reason = static_cast<wifi_event_sta_disconnected_t*>(event_data)->reason;
        if (self->connected_.exchange(false)) g_log_buffer.log("WiFi connection lost. Reconnecting...");
        // retries repeat every few seconds: report each cause once
        if (reason != self->last_reason_ && reason != WIFI_REASON_ASSOC_LEAVE) {
            self->last_reason_ = reason;
            g_log_buffer.log("WiFi not connected (reason " + std::to_string(reason) + ")" + reason_hint(reason));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = static_cast<ip_event_got_ip_t*>(event_data);
        char ip[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->ip_changed_ = self->ip_ != ip;
            self->ip_ = ip;
        }
        self->last_reason_ = 0;
        self->connected_ = true;
        self->announce_ = true;
        ESP_LOGI(TAG, "Connected to WiFi! IP: %s", ip);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        self->connected_ = false;
    }
}

std::string WiFiManager::get_ip() {
    if (!connected_) return ap_on_ ? AP_ADDRESS : "0.0.0.0";
    std::lock_guard<std::mutex> lock(mutex_);
    return ip_;
}

// Values the driver would refuse are replaced, so a bad setting can never lock the owner out
WiFiManager::Settings WiFiManager::load_settings() {
    Settings s;
    s.ssid = config_mgr_.get_string("wifi", "ssid", "");
    if (s.ssid == "YOUR_WIFI_SSID") s.ssid.clear();
    s.password = config_mgr_.get_string("wifi", "password", "");
    s.ap_fallback = config_mgr_.get_bool("wifi", "ap_fallback", true);
    s.ap_ssid = config_mgr_.get_string("wifi", "ap_ssid", DEFAULT_AP_SSID);
    if (s.ap_ssid.empty() || s.ap_ssid.size() > 32) s.ap_ssid = DEFAULT_AP_SSID;
    s.ap_password = config_mgr_.get_string("wifi", "ap_password", DEFAULT_AP_PASSWORD);
    if (!s.ap_password.empty() && (s.ap_password.size() < 8 || s.ap_password.size() > 63)) {
        g_log_buffer.log("Access point password must have 8-63 characters. Using the default one.");
        s.ap_password = DEFAULT_AP_PASSWORD;
    }
    return s;
}

void WiFiManager::apply_mode() {
    const wifi_mode_t mode = !ap_on_ ? WIFI_MODE_STA : cfg_.ssid.empty() ? WIFI_MODE_AP : WIFI_MODE_APSTA;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) ESP_LOGE(TAG, "Setting WiFi mode failed: %s", esp_err_to_name(err));
}

void WiFiManager::apply_station() {
    wifi_config_t wc = {};
    std::memcpy(wc.sta.ssid, cfg_.ssid.data(), std::min(cfg_.ssid.size(), sizeof(wc.sta.ssid)));
    std::memcpy(wc.sta.password, cfg_.password.data(), std::min(cfg_.password.size(), sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = cfg_.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) g_log_buffer.log(std::string("WiFi settings rejected: ") + esp_err_to_name(err));
}

void WiFiManager::apply_access_point() {
    wifi_config_t wc = {};
    std::memcpy(wc.ap.ssid, cfg_.ap_ssid.data(), cfg_.ap_ssid.size());
    wc.ap.ssid_len = cfg_.ap_ssid.size();
    std::memcpy(wc.ap.password, cfg_.ap_password.data(), cfg_.ap_password.size());
    wc.ap.channel = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode = cfg_.ap_password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) g_log_buffer.log(std::string("Access point settings rejected: ") + esp_err_to_name(err));
}

bool WiFiManager::start() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t* station = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(station, "polibot"); // how the router lists the device

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // config.json is the only place settings live
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this));

    cfg_ = load_settings();
    ap_on_ = cfg_.ssid.empty();
    apply_mode();
    if (ap_on_) apply_access_point();
    else apply_station();

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ap_on_ = false;
        ESP_LOGE(TAG, "WiFi failed to start: %s", esp_err_to_name(err));
        return false;
    }
    g_log_buffer.log(ap_on_ ? "No WiFi network configured. Join '" + cfg_.ap_ssid + "' and open http://" + AP_ADDRESS + "/"
                            : "Connecting to WiFi '" + cfg_.ssid + "'...");
    return xTaskCreate(task_entry, "wifi_task", 5120, this, 3, nullptr) == pdPASS;
}

void WiFiManager::task_entry(void* pv) {
    static_cast<WiFiManager*>(pv)->supervise();
}

void WiFiManager::reload() {
    const Settings next = load_settings();
    const bool station_changed = next.ssid != cfg_.ssid || next.password != cfg_.password;
    const bool ap_changed = next.ap_ssid != cfg_.ap_ssid || next.ap_password != cfg_.ap_password;
    cfg_ = next;
    if (station_changed) {
        g_log_buffer.log("WiFi settings changed. " + (cfg_.ssid.empty() ? std::string("Opening the setup access point.")
                                                                        : "Connecting to '" + cfg_.ssid + "'..."));
        connected_ = false; // first, so leaving on purpose is not reported as a lost connection
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500)); // the driver refuses new settings while it is still leaving
        last_reason_ = 0;
        if (cfg_.ssid.empty()) ap_on_ = true;
        apply_mode();
        if (!cfg_.ssid.empty()) apply_station();
        policy_.reset();
    }
    if (ap_on_ && (ap_changed || station_changed)) apply_access_point();
}

void WiFiManager::supervise() {
    int reload_in = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        // new settings apply a moment later, so the web page that saved them still gets its answer
        if (reload_.exchange(false)) reload_in = 2;
        if (reload_in > 0 && --reload_in == 0) reload();
        if (announce_.exchange(false) && online_cb_) online_cb_(ip_changed_);

        wifi_sta_list_t clients = {};
        if (ap_on_) esp_wifi_ap_get_sta_list(&clients);
        const auto act = policy_.tick({!cfg_.ssid.empty(), connected_, ap_on_, cfg_.ap_fallback, clients.num});
        if (act.start_ap || act.stop_ap) {
            ap_on_ = act.start_ap;
            apply_mode();
            if (ap_on_) apply_access_point();
            g_log_buffer.log(ap_on_ ? "Setup access point '" + cfg_.ap_ssid + "' opened: http://" + AP_ADDRESS + "/"
                                    : std::string("Setup access point closed."));
        }
        if (act.connect) esp_wifi_connect();
    }
}
