#pragma once

// Status colours of the on-board RGB LED. The decisions have no ESP-IDF in them, so they can be
// tested on a PC (see test/status_led_test.cpp).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct Rgb {
    uint8_t r, g, b;
};

enum class LinkStatus { NoWifi, NoIrc, SomeIrc, AllIrc };

inline LinkStatus link_status(bool wifi, size_t irc_up, size_t irc_total) {
    if (!wifi) return LinkStatus::NoWifi;
    if (irc_up == 0) return LinkStatus::NoIrc;
    return irc_up < irc_total ? LinkStatus::SomeIrc : LinkStatus::AllIrc;
}

// Who is on the web pages: nobody, someone with the status page on screen, or someone signed in to the panel
enum class WebUse { Nobody, Visitor, Panel };

constexpr int LED_PANEL = 4, LED_VISITOR = 5; // after one colour per LinkStatus: the quick flashes
constexpr int LED_COLOURS = 6;
constexpr const char* LED_KEYS[LED_COLOURS] = {"no_wifi", "no_irc", "some_irc", "all_irc", "panel", "visitor"};
constexpr int LED_MIN_PERIOD = 1, LED_MAX_PERIOD = 10; // seconds between two status blinks
constexpr unsigned LED_BLINK_TICKS = 5;                // how long a status blink lasts, in tenths of a second

// What the owner can change ("led" in the configuration)
struct LedSettings {
    bool enabled = true; // off: the LED stays dark
    int period = 2;      // seconds from one status blink to the next
    int brightness = 12; // percent; full power is dazzling
    Rgb colour[LED_COLOURS] = {
        {255, 0, 0},     // red: no WiFi
        {255, 50, 0},    // orange: WiFi, but no IRC network
        {255, 180, 0},   // yellow: some IRC networks
        {0, 255, 0},     // green: every IRC network
        {255, 255, 255}, // white: quick flashes while someone is signed in to the web panel
        {150, 0, 255},   // violet: quick flashes while someone has the status page on screen
    };
};

// "#rrggbb" <-> Rgb
inline bool parse_colour(const char* text, Rgb& out) {
    if (!text || std::strlen(text) != 7 || text[0] != '#' || std::strspn(text + 1, "0123456789abcdefABCDEF") != 6) return false;
    const unsigned long v = std::strtoul(text + 1, nullptr, 16);
    out = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    return true;
}

inline void format_colour(Rgb c, char (&out)[8]) {
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", c.r, c.g, c.b);
}

// What the LED shows. `tick` counts tenths of a second. The status colour blinks for half a second every
// `period` seconds; while someone is on the web pages, quick flashes fill the gap in between.
// The web pages draw the same pattern (showLed in web/panel_system.js and web/home.html).
inline Rgb led_colour(const LedSettings& set, LinkStatus status, WebUse web, unsigned tick) {
    const int p = !set.enabled || set.brightness < 0 ? 0 : set.brightness > 100 ? 100 : set.brightness;
    const int seconds = set.period < LED_MIN_PERIOD ? LED_MIN_PERIOD : set.period > LED_MAX_PERIOD ? LED_MAX_PERIOD : set.period;
    const unsigned cycle = seconds * 10, at = tick % cycle;
    // a short pause on both sides keeps the flashes apart from the status blink
    const bool flash = web != WebUse::Nobody && at >= LED_BLINK_TICKS + 2 && at + 2 < cycle && (at - LED_BLINK_TICKS) % 2 == 0;
    if (at >= LED_BLINK_TICKS && !flash) return {0, 0, 0};
    const Rgb c = set.colour[!flash ? static_cast<int>(status) : web == WebUse::Panel ? LED_PANEL : LED_VISITOR];
    // a part of a colour never dims to nothing, or dim orange would turn into red
    auto dim = [p](uint8_t v) { return uint8_t(v && p ? (v * p / 100 ? v * p / 100 : 1) : 0); };
    return {dim(c.r), dim(c.g), dim(c.b)};
}

// On the device: led_driver.cpp talks to the LED, status_led.cpp decides what it shows
struct cJSON;
bool led_driver_init();
void led_driver_show(Rgb colour);
void start_status_led();
cJSON* status_led_state_json();    // what the LED is showing now, for the web pages (caller owns it)
cJSON* status_led_settings_json(); // current settings and the defaults (caller owns it)
// Saves any of "brightness" and the colour keys. Empty string on success, else why not.
const char* status_led_save(const cJSON* changes);
