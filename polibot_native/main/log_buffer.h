#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct LogEntry {
    int64_t timestamp;
    std::string message;
};

class LogBuffer {
public:
    explicit LogBuffer(size_t max_lines = 60);

    void log(const std::string& msg);
    std::vector<LogEntry> get_lines();
    void clear();

private:
    size_t max_lines_;
    std::vector<LogEntry> lines_;
    std::mutex mutex_;
};

extern LogBuffer g_log_buffer;
