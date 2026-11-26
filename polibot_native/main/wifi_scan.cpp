#include "wifi_scan.h"
#include "psram_alloc.h"
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace {

constexpr int GIVE_UP_SECS = 15; // a scan that never reports back
constexpr uint16_t HEARD = 40;   // access points looked at, strongest first
constexpr size_t SHOWN = 24;     // networks listed

struct Network {
    char ssid[33];
    int8_t rssi;
    bool secure;
};

std::mutex s_mutex; // s_found
PsramVector<Network> s_found;
std::atomic<int64_t> s_started_at{-1}; // when the running scan began, -1: none

int64_t seconds() { return esp_timer_get_time() / 1000000; }

bool running() {
    const int64_t started = s_started_at;
    return started >= 0 && seconds() - started < GIVE_UP_SECS;
}

// Runs on the event task: its stack is small, so the records live on the heap
void on_scan_done(void*, esp_event_base_t, int32_t, void*) {
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    PsramVector<wifi_ap_record_t> records(std::min(count, HEARD));
    count = records.size();
    // either call releases the driver's own list
    if (!count || esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        count = 0;
    }
    PsramVector<Network> found;
    for (uint16_t i = 0; i < count && found.size() < SHOWN; i++) {
        const char* name = reinterpret_cast<const char*>(records[i].ssid);
        // hidden networks have no name; several access points share one
        if (!name[0] || std::any_of(found.begin(), found.end(), [name](const Network& n) { return std::strcmp(n.ssid, name) == 0; })) continue;
        Network n = {};
        std::strncpy(n.ssid, name, sizeof(n.ssid) - 1);
        n.rssi = records[i].rssi;
        n.secure = records[i].authmode != WIFI_AUTH_OPEN;
        found.push_back(n);
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    s_found.swap(found);
    s_started_at = -1;
}

} // namespace

const char* wifi_scan_start() {
    static bool listening = false;
    if (running()) return "";
    if (!listening) listening = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, on_scan_done, nullptr) == ESP_OK;
    wifi_mode_t mode;
    if (!listening || esp_wifi_get_mode(&mode) != ESP_OK) return "Looking for networks is not available.";
    // the setup access point alone cannot listen: the station side is switched on beside it
    if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);
    s_started_at = seconds(); // before starting: the scan may finish at once
    const esp_err_t err = esp_wifi_scan_start(nullptr, false);
    if (err == ESP_OK) return "";
    s_started_at = -1;
    return err == ESP_ERR_WIFI_STATE ? "The radio is busy joining a network." : "Looking for networks is not available.";
}

cJSON* wifi_scan_json() {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "scanning", running());
    cJSON* list = cJSON_AddArrayToObject(o, "networks");
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const Network& n : s_found) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", n.ssid);
        cJSON_AddNumberToObject(item, "rssi", n.rssi);
        cJSON_AddBoolToObject(item, "secure", n.secure);
        cJSON_AddItemToArray(list, item);
    }
    return o;
}
