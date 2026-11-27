#pragma once

// The newest log lines, in memory that a restart does not wipe (RTC memory on the device). After a
// power cut it holds noise, which the marks below tell apart.
#include <cstddef>
#include <cstdint>

struct SavedLine {
    int64_t time;
    uint32_t check;
    char text[148];
};

struct SavedLog {
    static constexpr size_t LINES = 24;
    uint32_t magic;
    uint32_t next; // where the next line goes
    SavedLine lines[LINES];
};

void saved_log_clear(SavedLog& log);
void saved_log_add(SavedLog& log, int64_t time, const char* text);
// The n-th line from the oldest (0 .. LINES-1), or null if that place holds none
const SavedLine* saved_log_line(const SavedLog& log, size_t n);
