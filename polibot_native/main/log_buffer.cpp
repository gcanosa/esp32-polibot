#include "log_buffer.h"
#include "crash_log.h"
#include <esp_attr.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <time.h>
#include <cstdio>

static const char* TAG = "IRC_LOG";
LogBuffer g_log_buffer(60);

// Not cleared at a start: the memory keeps it through a restart or a crash
static RTC_NOINIT_ATTR SavedLog s_saved;

static int64_t now_stamp() {
    time_t now = 0;
    time(&now);
    return (now > 1700000000) ? (int64_t)now : (esp_timer_get_time() / 1000000);
}

LogBuffer::LogBuffer(size_t max_lines) : max_lines_(max_lines) {}

void LogBuffer::restore(bool power_was_off) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; !power_was_off && i < SavedLog::LINES; i++) {
            if (const SavedLine* line = saved_log_line(s_saved, i)) lines_.push_back({line->time, next_id_++, line->text});
        }
        if (lines_.empty()) saved_log_clear(s_saved);
    }
    if (latest()) log("---- Restarted. The lines above are from before. ----");
}

void LogBuffer::log(const std::string& msg) {
    const int64_t ts = now_stamp();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back({ts, next_id_++, msg});
        if (lines_.size() > max_lines_) {
            lines_.erase(lines_.begin());
        }
        saved_log_add(s_saved, ts, msg.c_str());
    }
    ESP_LOGI(TAG, "%s", msg.c_str());
}

void LogBuffer::keep(const char* msg) {
    saved_log_add(s_saved, now_stamp(), msg);
}

void LogBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    saved_log_clear(s_saved);
}

uint32_t LogBuffer::latest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_id_ - 1;
}

std::vector<LogEntry> LogBuffer::get_lines(uint32_t after, uint32_t* oldest) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (oldest) *oldest = lines_.empty() ? next_id_ : lines_.front().id;
    std::vector<LogEntry> out;
    for (const auto& line : lines_) {
        if (line.id > after) out.push_back(line);
    }
    return out;
}
