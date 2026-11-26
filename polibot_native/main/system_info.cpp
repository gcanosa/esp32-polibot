#include "system_info.h"
#include "irc_client.h"
#include "status_led.h"
#include "config_manager.h"
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_ota_ops.h>
#include <esp_spiffs.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <soc/soc_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <atomic>
#include <string>
#if SOC_TEMP_SENSOR_SUPPORTED
#include <driver/temperature_sensor.h>
static temperature_sensor_handle_t s_thermometer = nullptr;
#endif

#if CONFIG_IDF_TARGET_ESP32S3
static const char* const CHIP_NAME = "ESP32-S3";
static const char* const CPU_CORE = "Xtensa LX7";
#else
static const char* const CHIP_NAME = "ESP32";
static const char* const CPU_CORE = "Xtensa LX6";
#endif

static const int SAMPLE_SECONDS = 2;
static std::atomic<int> s_load[portNUM_PROCESSORS];

// A core's load is the share of the last interval its idle task did not get
static void sample_load(void*) {
    static uint32_t idle_before[portNUM_PROCESSORS];
    static int64_t time_before = 0;
    const int64_t now = esp_timer_get_time(); // microseconds, like the run time counters
    const int64_t elapsed = now - time_before;
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        TaskStatus_t idle;
        const TaskHandle_t idle_task = xTaskGetIdleTaskHandleForCore(core);
        vTaskGetInfo(idle_task, &idle, pdFALSE, eRunning);
        const uint32_t idle_time = idle.ulRunTimeCounter - idle_before[core]; // survives the counter wrapping
        idle_before[core] = idle.ulRunTimeCounter;
        const int64_t busy = 100 - (int64_t)idle_time * 100 / (elapsed > 0 ? elapsed : 1);
        // run time is only counted at a task switch: no switch while idling the whole interval is not load
        const bool never_left_idle = idle_time == 0 && xTaskGetCurrentTaskHandleForCore(core) == idle_task;
        s_load[core] = never_left_idle || busy < 0 ? 0 : busy > 100 ? 100 : (int)busy;
    }
    time_before = now;
}

void system_info_start() {
    esp_timer_create_args_t args = {};
    args.callback = sample_load;
    args.name = "load";
    esp_timer_handle_t timer = nullptr;
    sample_load(nullptr);
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, SAMPLE_SECONDS * 1000000);
#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&config, &s_thermometer) != ESP_OK || temperature_sensor_enable(s_thermometer) != ESP_OK) {
        s_thermometer = nullptr;
    }
#endif
}

const char* restart_reason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "Power on or reset button";
        case ESP_RST_EXT: return "Reset pin";
        case ESP_RST_SW: return "Software restart";
        case ESP_RST_PANIC: return "Crash";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "Watchdog";
        case ESP_RST_BROWNOUT: return "Power supply dip (brownout)";
        case ESP_RST_USB: return "USB";
        default: return "Unknown";
    }
}

static void add_memory(cJSON* parent, const char* key, uint32_t capability) {
    cJSON* m = cJSON_AddObjectToObject(parent, key);
    cJSON_AddNumberToObject(m, "total", heap_caps_get_total_size(capability));
    cJSON_AddNumberToObject(m, "free", heap_caps_get_free_size(capability));
    cJSON_AddNumberToObject(m, "min_free", heap_caps_get_minimum_free_size(capability));
}

cJSON* system_info_json() {
    const esp_app_desc_t* app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    esp_flash_get_size(nullptr, &flash_bytes);

    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", app->version);
    cJSON_AddStringToObject(o, "built", (std::string(app->date) + " " + app->time).c_str());
    const esp_partition_t* slot = esp_ota_get_running_partition();
    cJSON_AddStringToObject(o, "slot", slot ? slot->label : "");

    cJSON_AddStringToObject(o, "chip", CHIP_NAME);
    cJSON_AddStringToObject(o, "revision", ("v" + std::to_string(chip.revision / 100) + "." + std::to_string(chip.revision % 100)).c_str());
    cJSON_AddNumberToObject(o, "cores", chip.cores);
    cJSON_AddNumberToObject(o, "cpu_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(o, "flash_bytes", flash_bytes);
    cJSON* radio = cJSON_AddArrayToObject(o, "radio");
    if (chip.features & CHIP_FEATURE_WIFI_BGN) cJSON_AddItemToArray(radio, cJSON_CreateString("Wi-Fi 2.4 GHz"));
    if (chip.features & CHIP_FEATURE_BLE) cJSON_AddItemToArray(radio, cJSON_CreateString("Bluetooth LE"));
    cJSON_AddStringToObject(o, "idf", esp_get_idf_version());

    cJSON* load = cJSON_AddArrayToObject(o, "cpu_load");
    for (int core = 0; core < portNUM_PROCESSORS; core++) cJSON_AddItemToArray(load, cJSON_CreateNumber(s_load[core]));
    add_memory(o, "ram", MALLOC_CAP_INTERNAL);
    add_memory(o, "psram", MALLOC_CAP_SPIRAM);
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        cJSON* s = cJSON_AddObjectToObject(o, "storage");
        cJSON_AddNumberToObject(s, "total", total);
        cJSON_AddNumberToObject(s, "used", used);
    }
#if SOC_TEMP_SENSOR_SUPPORTED
    float celsius = 0;
    if (s_thermometer && temperature_sensor_get_celsius(s_thermometer, &celsius) == ESP_OK) {
        cJSON_AddNumberToObject(o, "temperature_c", celsius);
    }
#endif
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(o, "wifi_rssi", ap.rssi);

    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "restart_reason", restart_reason());
    cJSON_AddNumberToObject(o, "tasks", uxTaskGetNumberOfTasks());

    size_t networks = 0, connected = 0, channels = 0;
    for (const auto& client : g_irc_manager.clients()) {
        networks++;
        connected += client->is_registered();
        channels += client->get_joined_channels().size();
    }
    cJSON_AddItemToObject(o, "led", status_led_state_json());

    cJSON* irc = cJSON_AddObjectToObject(o, "irc");
    cJSON_AddNumberToObject(irc, "networks", networks);
    cJSON_AddNumberToObject(irc, "connected", connected);
    cJSON_AddNumberToObject(irc, "channels", channels);
    return o;
}

std::string system_info_line() {
    cJSON* info = system_info_json();
    auto number = [](const cJSON* o, const char* key) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
        return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    };
    auto text = [info](const char* key) -> const char* {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(info, key);
        return cJSON_IsString(v) ? v->valuestring : "?";
    };
    const cJSON* ram = cJSON_GetObjectItemCaseSensitive(info, "ram");
    const cJSON* psram = cJSON_GetObjectItemCaseSensitive(info, "psram");
    const cJSON* storage = cJSON_GetObjectItemCaseSensitive(info, "storage");
    cJSON* name = g_config_mgr.get_item("device_name");

    std::string load;
    const cJSON* core = nullptr;
    cJSON_ArrayForEach(core, cJSON_GetObjectItemCaseSensitive(info, "cpu_load")) {
        load += (load.empty() ? "" : "/") + std::to_string(core->valueint) + "%";
    }

    char buf[160];
    std::string line = "Sysinfo for '" + std::string(cJSON_IsString(name) ? name->valuestring : "PoliBOT") + "': ";
    snprintf(buf, sizeof(buf), "PoliBOT v%s on ESP-IDF %s (FreeRTOS) powered by %s %s, ", text("version"), text("idf"), text("chip"), text("revision"));
    line += buf;
    snprintf(buf, sizeof(buf), "CPU: %dx %s @ %d MHz (load %s), ", (int)number(info, "cores"), CPU_CORE, (int)number(info, "cpu_mhz"), load.c_str());
    line += buf;
    snprintf(buf, sizeof(buf), "RAM: %d/%d KB, ", (int)((number(ram, "total") - number(ram, "free")) / 1024), (int)(number(ram, "total") / 1024));
    line += buf;
    if (number(psram, "total") > 0) {
        snprintf(buf, sizeof(buf), "PSRAM: %.1f/%.1f MB, ", (number(psram, "total") - number(psram, "free")) / 1048576, number(psram, "total") / 1048576);
        line += buf;
    }
    snprintf(buf, sizeof(buf), "Flash: %d MB (settings %d/%d KB), ", (int)(number(info, "flash_bytes") / 1048576),
             (int)(number(storage, "used") / 1024), (int)(number(storage, "total") / 1024));
    line += buf;
    if (cJSON_GetObjectItemCaseSensitive(info, "temperature_c")) {
        snprintf(buf, sizeof(buf), "Temp: %.1f \xc2\xb0""C, ", number(info, "temperature_c"));
        line += buf;
    }
    if (cJSON_GetObjectItemCaseSensitive(info, "wifi_rssi")) line += "WiFi: " + std::to_string((int)number(info, "wifi_rssi")) + " dBm, ";
    snprintf(buf, sizeof(buf), "%d tasks, %.2fh up", (int)number(info, "tasks"), number(info, "uptime_s") / 3600);
    line += buf;

    cJSON_Delete(name);
    cJSON_Delete(info);
    return line;
}
