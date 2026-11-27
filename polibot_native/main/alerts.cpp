#include "alerts.h"
#include "bot_settings.h"
#include "config_manager.h"
#include "irc_client.h"
#include "log_buffer.h"
#include "wifi_manager.h"
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace {

constexpr size_t WAITING = 8;
constexpr int64_t RETRY_AFTER = 60; // seconds

struct Away {
    std::string network;
    int64_t since;
    bool told;
};

std::mutex s_mutex;
std::deque<std::string> s_queue; // guarded by s_mutex
std::atomic<bool> s_sending{false};
std::atomic<int64_t> s_retry_at{0};
std::vector<Away> s_away; // the heartbeat task only

int64_t seconds() { return esp_timer_get_time() / 1000000; }

bool usable(const std::string& url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }

bool post(const std::string& url, const std::string& format, const std::string& message) {
    std::string body = message;
    if (format == "json") {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "text", message.c_str());
        cJSON_AddStringToObject(o, "content", message.c_str());
        char* printed = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (!printed) return false;
        body = printed;
        cJSON_free(printed);
    }
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Content-Type", format == "json" ? "application/json" : "text/plain; charset=utf-8");
    esp_http_client_set_post_field(client, body.data(), body.size());
    const bool ok = esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) / 100 == 2;
    esp_http_client_cleanup(client);
    return ok;
}

void send_waiting() { // everything it holds is freed on return
    for (;;) {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_queue.empty()) return;
            message = s_queue.front();
        }
        const auto settings = g_settings.get();
        if (!usable(settings->alert_url)) return;
        // the address is not logged: it often holds the secret that protects it
        if (!post(settings->alert_url, settings->alert_format, message)) {
            if (!s_retry_at) g_log_buffer.log("Could not send an alert. Will keep trying.");
            s_retry_at = seconds() + RETRY_AFTER;
            return;
        }
        s_retry_at = 0;
        g_log_buffer.log("Alert sent: " + message);
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_queue.empty() && s_queue.front() == message) s_queue.pop_front();
    }
}

void sender(void*) {
    send_waiting();
    s_sending = false;
    vTaskDelete(nullptr);
}

void watch_networks(int minutes) {
    const auto clients = g_irc_manager.clients();
    const int64_t now = seconds();
    // networks that are gone from the settings
    s_away.erase(std::remove_if(s_away.begin(), s_away.end(), [&](const Away& a) {
        return std::none_of(clients.begin(), clients.end(), [&](const auto& c) { return c->name() == a.network; });
    }), s_away.end());
    for (const auto& client : clients) {
        auto it = std::find_if(s_away.begin(), s_away.end(), [&](const Away& a) { return a.network == client->name(); });
        if (client->is_registered()) {
            if (it == s_away.end()) continue;
            if (it->told) alert(client->name() + " is back on IRC after " + format_duration(now - it->since) + ".");
            s_away.erase(it);
        } else if (it == s_away.end()) {
            s_away.push_back({client->name(), now, false});
        } else if (minutes && !it->told && now - it->since >= minutes * 60) {
            it->told = true;
            alert(client->name() + " has been off IRC for " + format_duration(now - it->since) + ".");
        }
    }
}

} // namespace

void alert(const std::string& message) {
    if (!usable(g_settings.get()->alert_url)) return;
    cJSON* name = g_config_mgr.get_item("device_name");
    const std::string line = std::string(cJSON_IsString(name) ? name->valuestring : "PoliBOT") + ": " + message;
    cJSON_Delete(name);
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_queue.size() >= WAITING) s_queue.pop_front();
    s_queue.push_back(line);
}

void alerts_tick() {
    const auto settings = g_settings.get();
    if (!usable(settings->alert_url)) {
        s_away.clear();
        std::lock_guard<std::mutex> lock(s_mutex);
        s_queue.clear();
        return;
    }
    watch_networks(settings->alert_offline_minutes);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_queue.empty()) return;
    }
    if (!g_wifi_mgr.is_connected() || seconds() < s_retry_at || s_sending.exchange(true)) return;
    // lives only while sending; the stack must hold a TLS handshake
    if (xTaskCreatePinnedToCore(sender, "alert", 9216, nullptr, 2, nullptr, 1) != pdPASS) s_sending = false;
}
