#include "health.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <climits>
#include "bot_settings.h"
#include "irc_client.h"
#include "system_info.h"

void report_restart() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool fault = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                       reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;
    g_irc_manager.report(ReportKind::system, std::string(fault ? "Restarted after a fault: " : "Started: ") + restart_reason() + ".");
}

void check_memory() {
    static unsigned reported = UINT_MAX;
    const unsigned limit = g_settings.get()->low_memory_kb;
    const unsigned lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
    // the lowest figure only ever falls: say so once for every further 4 KB below the limit
    if (!limit || lowest >= limit || lowest + 4 > reported) return;
    reported = lowest;
    const unsigned now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    g_irc_manager.report(ReportKind::system, "Low memory: internal RAM went down to " + std::to_string(lowest) + " KB free (now " +
                                                 std::to_string(now) + " KB).");
}
