#include "common/clock.hpp"

#include <windows.h>

namespace ast {

SteadyClock& SteadyClock::instance() {
    static SteadyClock c;
    return c;
}

void SteadyClock::init() {
    LARGE_INTEGER f;
    if (QueryPerformanceFrequency(&f)) {
        freq_ = static_cast<uint64_t>(f.QuadPart);
        initialized_ = true;
    } else {
        // Fall back to a 1MHz assumption; timings will be approximate.
        freq_ = 1'000'000;
        initialized_ = true;
    }
}

uint64_t SteadyClock::now_ns() const {
    LARGE_INTEGER c;
    if (!initialized_ || !QueryPerformanceCounter(&c)) return 0;
    // c.QuadPart / freq_ seconds -> nanoseconds.
    return (static_cast<uint64_t>(c.QuadPart) * 1'000'000'000ULL) / freq_;
}

namespace {
uint64_t filetime_to_unix_us(FILETIME ft) {
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    // FILETIME is 100ns intervals since 1601-01-01.
    constexpr uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    return (li.QuadPart - EPOCH_DIFF_100NS) / 10;
}
} // namespace

uint64_t wall_time_us() {
    FILETIME ft;
    // Precise time is available since Windows 8 / Server 2012.
    using PreciseFn = void(WINAPI*)(LPFILETIME);
    static PreciseFn precise = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        if (!k) return (PreciseFn)nullptr;
        return (PreciseFn)(void*)GetProcAddress(k, "GetSystemTimePreciseAsFileTime");
    }();
    if (precise) {
        precise(&ft);
    } else {
        GetSystemTimeAsFileTime(&ft);
    }
    return filetime_to_unix_us(ft);
}

uint64_t wall_time_ms() { return wall_time_us() / 1000; }
uint64_t wall_time_s() { return wall_time_us() / 1'000'000; }

uint64_t SystemClock::now_ns() const { return SteadyClock::instance().now_ns(); }
uint64_t SystemClock::now_wall_us() const { return wall_time_us(); }

} // namespace ast
