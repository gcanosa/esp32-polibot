#pragma once

// Containers for the bot's lists live in the external RAM when there is any, so that the internal
// one stays free for Wi-Fi, TLS and task stacks. On a PC (tests) they use the ordinary heap.
#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

inline void* psram_malloc(size_t bytes) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
#else
    return std::malloc(bytes);
#endif
}

template <class T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <class U> PsramAllocator(const PsramAllocator<U>&) {}
    T* allocate(size_t count) {
        void* block = psram_malloc(count * sizeof(T));
        if (!block) std::abort(); // as the default allocator does without exceptions
        return static_cast<T*>(block);
    }
    void deallocate(T* block, size_t) { std::free(block); } // free() knows every ESP-IDF heap
};
template <class T, class U> bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) { return true; }
template <class T, class U> bool operator!=(const PsramAllocator<T>&, const PsramAllocator<U>&) { return false; }

template <class T> using PsramVector = std::vector<T, PsramAllocator<T>>;
using PsramString = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
