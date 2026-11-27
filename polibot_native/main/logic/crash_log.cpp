#include "crash_log.h"
#include <cstring>

namespace {

constexpr uint32_t MAGIC = 0x504c4f47;

uint32_t checksum(const SavedLine& line) { // FNV-1a, never 0
    uint32_t hash = 2166136261u;
    auto add = [&](const void* data, size_t length) {
        for (size_t i = 0; i < length; i++) hash = (hash ^ static_cast<const unsigned char*>(data)[i]) * 16777619u;
    };
    add(&line.time, sizeof(line.time));
    add(line.text, sizeof(line.text));
    return hash ? hash : 1;
}

bool valid(const SavedLog& log) { return log.magic == MAGIC && log.next < SavedLog::LINES; }

} // namespace

void saved_log_clear(SavedLog& log) {
    std::memset(&log, 0, sizeof(log));
    log.magic = MAGIC;
}

void saved_log_add(SavedLog& log, int64_t time, const char* text) {
    if (!valid(log)) saved_log_clear(log);
    SavedLine& line = log.lines[log.next];
    log.next = (log.next + 1) % SavedLog::LINES;
    size_t length = std::strlen(text);
    if (length >= sizeof(line.text)) {
        length = sizeof(line.text) - 1;
        while (length && (text[length] & 0xc0) == 0x80) length--; // not in the middle of a character
    }
    line.check = 0; // half written is not valid
    line.time = time;
    std::memset(line.text, 0, sizeof(line.text));
    std::memcpy(line.text, text, length);
    line.check = checksum(line);
}

const SavedLine* saved_log_line(const SavedLog& log, size_t n) {
    if (!valid(log) || n >= SavedLog::LINES) return nullptr;
    const SavedLine& line = log.lines[(log.next + n) % SavedLog::LINES];
    return line.check && line.check == checksum(line) && !line.text[sizeof(line.text) - 1] ? &line : nullptr;
}
