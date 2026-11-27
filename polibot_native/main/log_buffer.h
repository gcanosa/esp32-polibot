#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct LogEntry {
    int64_t timestamp;
    uint32_t id; // counts up from 1 since this start
    std::string message;
};

// The event log in the panel. The newest lines are also kept in RTC memory, so that after a crash
// it can still be read what led to it.
class LogBuffer {
public:
    explicit LogBuffer(size_t max_lines = 60);

    // Call before the first line: brings back what was logged before a restart, unless the power was off
    void restore(bool power_was_off);
    void log(const std::string& msg);
    // Writes to the kept lines only and takes no lock: for the last words before a deliberate crash
    void keep(const char* msg);
    // The lines after the one numbered `after`; `oldest` gets the number of the oldest line still held
    std::vector<LogEntry> get_lines(uint32_t after = 0, uint32_t* oldest = nullptr);
    uint32_t latest(); // the number of the newest line, 0 if there never was one
    void clear();

private:
    size_t max_lines_;
    std::vector<LogEntry> lines_;
    uint32_t next_id_ = 1;
    std::mutex mutex_;
};

extern LogBuffer g_log_buffer;
