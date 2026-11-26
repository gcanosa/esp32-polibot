#pragma once

// Lines waiting their turn, kept in one string: no allocation per line and a hard limit on bytes
#include <string>
#include "psram_alloc.h"

class LineQueue {
public:
    explicit LineQueue(size_t max_bytes) : max_bytes_(max_bytes) {}

    bool push(const std::string& line) { // false if it does not fit; a line holds no '\n'
        if (data_.size() + line.size() + 1 > max_bytes_) return false;
        data_.append(line.data(), line.size());
        data_ += '\n';
        return true;
    }

    void push_dropping_oldest(const std::string& line) {
        std::string dropped;
        while (!push(line) && pop(dropped)) {}
    }

    bool pop(std::string& line) {
        if (data_.empty()) return false;
        const size_t end = data_.find('\n');
        line.assign(data_.data(), end);
        data_.erase(0, end + 1);
        if (data_.empty()) data_.shrink_to_fit();
        return true;
    }

    bool empty() const { return data_.empty(); }
    void clear() {
        data_.clear();
        data_.shrink_to_fit();
    }

private:
    PsramString data_;
    size_t max_bytes_;
};
