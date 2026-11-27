#include "health.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include "alerts.h"
#include "bot_settings.h"
#include "irc_client.h"
#include "log_buffer.h"
#include "system_info.h"

// The check takes no locks: a task that hangs may be holding one
namespace {

constexpr int SLOTS = 12; // four networks, connections still closing, the main loop and the web panel
enum { FREE, CLAIMED, WATCHED };

struct Watched {
    std::atomic<int> state{FREE};
    std::atomic<uint32_t> seen{0}; // seconds since boot
    char name[40];
};
Watched s_watched[SLOTS];
std::atomic<int> s_limit_minutes{5};

uint32_t now_seconds() { return esp_timer_get_time() / 1000000; }

void check_hangs(void*) {
    const int minutes = s_limit_minutes;
    if (!minutes) return;
    for (auto& w : s_watched) {
        if (w.state != WATCHED || now_seconds() - w.seen < (uint32_t)minutes * 60) continue;
        char why[96];
        snprintf(why, sizeof(why), "%s has not responded for %d minute%s. Restarting.", w.name, minutes, minutes == 1 ? "" : "s");
        g_log_buffer.keep(why);
        // not esp_restart(): saving on the way down could wait for the very lock that is stuck
        abort();
    }
}

} // namespace

int watch_start(const char* name) {
    for (int i = 0; i < SLOTS; i++) {
        int expected = FREE;
        if (!s_watched[i].state.compare_exchange_strong(expected, CLAIMED)) continue;
        snprintf(s_watched[i].name, sizeof(s_watched[i].name), "%s", name);
        s_watched[i].seen = now_seconds();
        s_watched[i].state = WATCHED; // only now does the check look at it
        return i;
    }
    return -1;
}

void watch_beat(int slot) {
    if (slot >= 0) s_watched[slot].seen = now_seconds();
}

void watch_stop(int slot) {
    if (slot >= 0) s_watched[slot].state = FREE;
}

void watch_limit(int minutes) { s_limit_minutes = minutes; }

void watch_hangs() {
    const esp_timer_create_args_t args = {.callback = check_hangs, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                          .name = "hangs", .skip_unhandled_events = true};
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_periodic(timer, 10 * 1000000);
}

void report_restart() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool fault = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                       reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;
    const std::string text = std::string(fault ? "Restarted after a fault: " : "Started: ") + restart_reason() + ".";
    g_irc_manager.report(ReportKind::system, text);
    if (fault && g_settings.get()->alert_faults) alert(text);
}

void check_memory() {
    static unsigned reported = UINT_MAX;
    const unsigned limit = g_settings.get()->low_memory_kb;
    const unsigned lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
    // the lowest figure only ever falls: say so once for every further 4 KB below the limit
    if (!limit || lowest >= limit || lowest + 4 > reported) return;
    reported = lowest;
    const unsigned now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    const std::string text = "Low memory: internal RAM went down to " + std::to_string(lowest) + " KB free (now " + std::to_string(now) + " KB).";
    g_irc_manager.report(ReportKind::system, text);
    if (g_settings.get()->alert_memory) alert(text);
}
