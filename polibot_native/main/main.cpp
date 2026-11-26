#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config_manager.h"
#include "security_manager.h"
#include "wifi_manager.h"
#include "ntp_manager.h"
#include "guardian.h"
#include "irc_client.h"
#include "web_server.h"
#include "display_interface.h"
#include "log_buffer.h"
#include "ota_manager.h"
#include "status_led.h"
#include "system_info.h"
#include "bot_settings.h"
#include "bot_memory.h"
#include "psram_alloc.h"
#include "health.h"

static const char* TAG = "MAIN";

// Defined here, in dependency order: constructors call into earlier objects, and C++
// only guarantees initialisation order within a single translation unit.
ConfigManager g_config_mgr("/spiffs/config.json");
SettingsStore g_settings(g_config_mgr); // before the others: they read it when settings change
SecurityManager g_security_mgr(g_config_mgr);
WiFiManager g_wifi_mgr(g_config_mgr);
NtpManager g_ntp_mgr(g_config_mgr);
GuardianEngine g_guardian(g_config_mgr, g_security_mgr);
IRCManager g_irc_manager(g_config_mgr, g_security_mgr, g_guardian);
WebServer g_web_server(g_config_mgr, g_security_mgr, g_irc_manager);


// Writes the "seen" list, waiting messages, bans and timed voice; the two lists only at their interval unless forced
static void save_memory(bool force) {
    if (!g_memory.save("/spiffs", bot_clock(), g_settings.get()->seen_save_minutes * 60, force)) {
        g_log_buffer.log("Could not save the bot's memory (seen, messages, bans, voice) to flash.");
    }
}

static void save_before_restart() {
    g_config_mgr.flush(); // settings changed on IRC that are not in flash yet
    save_memory(true);
}

static void heartbeat_task(void* pv) {
    const bool web_ok = (pv != nullptr);
    int ticks = 0;
    int beats = 0;
    constexpr int64_t BEAT = 10 * 1000000; // microseconds
    int64_t next_beat = esp_timer_get_time() + BEAT;
    bool save_failed = false;
    while (true) {
        // every ten seconds, and woken in between to write settings an IRC connection has changed
        const int64_t wait = next_beat - esp_timer_get_time();
        if (wait > 0) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait / 1000) + 1);
        const bool saved = g_config_mgr.flush();
        if (!saved && !save_failed) g_irc_manager.report(ReportKind::system, "Could not save the settings to flash. Will keep trying.");
        save_failed = !saved;
        if (esp_timer_get_time() < next_beat) continue;
        next_beat = esp_timer_get_time() + BEAT;
        // A freshly flashed image is kept only after 30 s of stable runtime; a crash or reset before that rolls back.
        if (web_ok && g_wifi_mgr.reachable() && ticks < 3 && ++ticks == 3) g_ota_mgr.confirm_running_image();
        uint32_t free_kb = esp_get_free_heap_size() / 1024;
        int64_t uptime_sec = esp_timer_get_time() / 1000000;
        g_display_mgr.update_system_stats(free_kb, (uint32_t)uptime_sec);
        ESP_LOGD(TAG, "Heartbeat: Free Heap: %u KB, Uptime: %lld s", (unsigned int)free_kb, (long long)uptime_sec);
        if (++beats % 360 == 0) { // once an hour
            const auto settings = g_settings.get();
            g_memory.seen.forget_older_than(bot_clock(), settings->seen_days);
            g_memory.mail.forget_older_than(bot_clock(), settings->tell_days);
            g_memory.voices.forget_older_than(bot_clock(), settings->voice_days);
        }
        save_memory(false);
        check_memory();
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=======================================================");
    ESP_LOGI(TAG, "   PoliBOT Sentry - ESP32-S3 Native C/C++ Engine       ");
    ESP_LOGI(TAG, "=======================================================");
    ESP_LOGI(TAG, "Version %s", esp_app_get_description()->version);

    // The configuration tree and every JSON message live in external RAM (before the first cJSON call)
    cJSON_Hooks json_memory = {psram_malloc, free};
    cJSON_InitHooks(&json_memory);

    // 1. Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize SPIFFS Partition for config.json storage
    g_config_mgr.init_spiffs();
    g_config_mgr.load();
    start_status_led(); // red until WiFi is up

    // 3. Initialize Core Subsystems
    g_settings.load();
    g_memory.load("/spiffs", bot_clock());
    esp_register_shutdown_handler(save_before_restart); // restarts from IRC, the panel and updates
    g_security_mgr.load();
    g_guardian.load_rules();
    g_ntp_mgr.init();
    g_ota_mgr.init();

    g_log_buffer.log("Subsystems initialized. Connecting WiFi...");

    // 4. Start WiFi (station, or the setup access point). Reconnects by itself from here on.
    g_wifi_mgr.on_online([](bool ip_changed) {
        g_log_buffer.log("WiFi connected. Web panel: http://" + g_wifi_mgr.get_ip() + "/");
        g_ntp_mgr.start();
        g_irc_manager.network_up(ip_changed);
    });
    g_wifi_mgr.start();

    // 5. Start Web Control Panel on port 80
    system_info_start();
    bool web_ok = g_web_server.start(80);

    // 6. Start one IRC task per configured network
    g_irc_manager.start();
    report_restart(); // waits in each network's queue until its report channel is ready

    // 7. Spawn Periodic Heartbeat
    TaskHandle_t heartbeat = nullptr;
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat_task", 6144, (void*)web_ok, 1, &heartbeat, 1);
    g_config_mgr.set_writer(heartbeat); // until then, and if it could not start, every task writes for itself

    ESP_LOGI(TAG, "PoliBOT Sentry running on dual-core Xtensa LX7.");
}
