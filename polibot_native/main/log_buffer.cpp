#include "log_buffer.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <time.h>
#include <cstdio>

static const char* TAG = "IRC_LOG";
LogBuffer g_log_buffer(60);

LogBuffer::LogBuffer(size_t max_lines) : max_lines_(max_lines) {}

void LogBuffer::log(const std::string& msg) {
    time_t now = 0;
    time(&now);
    int64_t ts = (now > 1700000000) ? (int64_t)now : (esp_timer_get_time() / 1000000);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back({ts, msg});
        if (lines_.size() > max_lines_) {
            lines_.erase(lines_.begin());
        }
    }
    ESP_LOGI(TAG, "%s", msg.c_str());
}

void LogBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
}

std::vector<LogEntry> LogBuffer::get_lines() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
}
