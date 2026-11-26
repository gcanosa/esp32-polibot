#pragma once

#include <cstdint>

struct Clock {
    int64_t steady; // seconds since boot
    int64_t epoch;  // wall clock, 0 while it is not synchronised
};
