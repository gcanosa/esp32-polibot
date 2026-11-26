#include "status_led.h"
#include "irc_client.h"
#include "web_server.h"
#include "wifi_manager.h"
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "LED";

static LinkStatus current_status() {
    const auto clients = g_irc_manager.clients();
    size_t up = 0;
    for (const auto& c : clients) up += c->is_registered();
    return link_status(g_wifi_mgr.is_connected(), up, clients.size());
}

// Anything missing or unreadable keeps its default
static LedSettings load_settings() {
    LedSettings s;
    s.enabled = g_config_mgr.get_bool("led", "enabled", s.enabled);
    s.period = g_config_mgr.get_int("led", "period", s.period);
    s.brightness = g_config_mgr.get_int("led", "brightness", s.brightness);
    for (int i = 0; i < LED_COLOURS; i++) parse_colour(g_config_mgr.get_string("led", LED_KEYS[i]).c_str(), s.colour[i]);
    return s;
}

static void add_colour(cJSON* object, const char* key, Rgb colour) {
    char text[8];
    format_colour(colour, text);
    cJSON_AddStringToObject(object, key, text);
}

static void add_settings(cJSON* object, const LedSettings& s) {
    cJSON_AddBoolToObject(object, "enabled", s.enabled);
    cJSON_AddNumberToObject(object, "period", s.period);
    cJSON_AddNumberToObject(object, "brightness", s.brightness);
    for (int i = 0; i < LED_COLOURS; i++) add_colour(object, LED_KEYS[i], s.colour[i]);
}

static WebUse web_use() {
    return g_web_server.in_use() ? WebUse::Panel : g_web_server.visited() ? WebUse::Visitor : WebUse::Nobody;
}

// The pages draw this and blink by themselves, so mirroring the LED costs no extra requests
cJSON* status_led_state_json() {
    const LedSettings s = load_settings();
    const LinkStatus status = current_status();
    const WebUse web = web_use();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", LED_KEYS[static_cast<int>(status)]);
    add_colour(o, "colour", s.colour[static_cast<int>(status)]);
    cJSON_AddStringToObject(o, "web", web == WebUse::Panel ? "panel" : web == WebUse::Visitor ? "visitor" : "");
    add_colour(o, "blink_colour", s.colour[web == WebUse::Visitor ? LED_VISITOR : LED_PANEL]);
    cJSON_AddNumberToObject(o, "period", s.period);
    cJSON_AddBoolToObject(o, "on", s.enabled && s.brightness > 0);
    return o;
}

cJSON* status_led_settings_json() {
    cJSON* o = cJSON_CreateObject();
    add_settings(o, load_settings());
    add_settings(cJSON_AddObjectToObject(o, "defaults"), LedSettings());
    return o;
}

// Saved without a rehash: that would sign everybody out on IRC just to change a colour
const char* status_led_save(const cJSON* changes) {
    const cJSON* brightness = cJSON_GetObjectItemCaseSensitive(changes, "brightness");
    if (brightness && (!cJSON_IsNumber(brightness) || brightness->valuedouble < 0 || brightness->valuedouble > 100)) {
        return "Brightness goes from 0 to 100.";
    }
    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(changes, "enabled");
    if (enabled && !cJSON_IsBool(enabled)) return "The LED is switched on or off.";
    const cJSON* period = cJSON_GetObjectItemCaseSensitive(changes, "period");
    if (period && (!cJSON_IsNumber(period) || period->valuedouble < LED_MIN_PERIOD || period->valuedouble > LED_MAX_PERIOD)) {
        return "The LED blinks every 1 to 10 seconds.";
    }
    Rgb ignored;
    for (const char* key : LED_KEYS) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(changes, key);
        if (v && (!cJSON_IsString(v) || !parse_colour(v->valuestring, ignored))) return "Colours are written like #ff8800.";
    }
    const bool saved = g_config_mgr.update([changes](cJSON* root) {
        cJSON* led = cJSON_GetObjectItemCaseSensitive(root, "led");
        if (!cJSON_IsObject(led)) {
            cJSON_DeleteItemFromObjectCaseSensitive(root, "led");
            led = cJSON_AddObjectToObject(root, "led");
        }
        auto copy = [&](const char* key) {
            const cJSON* v = cJSON_GetObjectItemCaseSensitive(changes, key);
            if (!v) return;
            cJSON_DeleteItemFromObjectCaseSensitive(led, key);
            cJSON_AddItemToObject(led, key, cJSON_Duplicate(v, 1));
        };
        copy("enabled");
        copy("period");
        copy("brightness");
        for (const char* key : LED_KEYS) copy(key);
        return true;
    });
    return saved ? "" : "Could not save to flash.";
}

static void status_led_task(void*) {
    static const char* const NAMES[] = {"no WiFi", "no IRC network", "some IRC networks", "all IRC networks"};
    static const char* const USERS[] = {"", " (status page open)", " (web panel signed in)"};
    LedSettings settings;
    int shown_state = -1;
    for (unsigned tick = 0;; tick++) {
        if (!settings.enabled || tick % 10 == 0) settings = load_settings(); // changes apply within a second

        if (!settings.enabled) { // dark, and nothing to work out: only watch for being switched on again
            if (shown_state != -2) ESP_LOGI(TAG, "Status LED switched off.");
            shown_state = -2;
            led_driver_show({0, 0, 0});
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        const LinkStatus status = current_status();
        const WebUse web = web_use();
        led_driver_show(led_colour(settings, status, web, tick));

        const int state = static_cast<int>(status) * 3 + static_cast<int>(web);
        if (state != shown_state) {
            shown_state = state;
            ESP_LOGI(TAG, "Status: %s%s", NAMES[static_cast<int>(status)], USERS[static_cast<int>(web)]);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void start_status_led() {
    if (!led_driver_init()) {
        ESP_LOGE(TAG, "Could not set up the RGB LED.");
        return;
    }
    xTaskCreatePinnedToCore(status_led_task, "status_led", 4096, nullptr, 1, nullptr, 1);
}
