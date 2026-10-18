// perf_counters.hpp - five hardware counters around a timed region via perf_event_open (Linux only).
// Per-process, user-space only (exclude_kernel), so it works at perf_event_paranoid <= 2 without root.
// If any counter cannot be opened it reports -1 for that column and the run continues; nothing else changes.
#pragma once
#include <cstdint>
#include <cstring>
#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

struct PerfCounters {
    static constexpr int NC = 5;
    static constexpr const char* names[NC] = {"instructions", "cycles", "l1d_read_miss", "llc_read_miss", "dtlb_read_miss"};
    int fd[NC]; long long val[NC];
    PerfCounters() { for (int i = 0; i < NC; i++) { fd[i] = -1; val[i] = -1; }
#ifdef __linux__
        struct { uint32_t type; uint64_t config; } spec[NC] = {
            {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS}, {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
            {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
            {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_LL  | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
            {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)}};
        for (int i = 0; i < NC; i++) {
            perf_event_attr a; std::memset(&a, 0, sizeof a); a.type = spec[i].type; a.size = sizeof a; a.config = spec[i].config;
            a.disabled = 1; a.exclude_kernel = 1; a.exclude_hv = 1;
            fd[i] = (int)syscall(__NR_perf_event_open, &a, 0, -1, -1, 0);
        }
#endif
    }
    ~PerfCounters() {
#ifdef __linux__
        for (int i = 0; i < NC; i++) if (fd[i] >= 0) close(fd[i]);
#endif
    }
    bool any() const { for (int i = 0; i < NC; i++) if (fd[i] >= 0) return true; return false; }
    void start() {
#ifdef __linux__
        for (int i = 0; i < NC; i++) if (fd[i] >= 0) { ioctl(fd[i], PERF_EVENT_IOC_RESET, 0); ioctl(fd[i], PERF_EVENT_IOC_ENABLE, 0); }
#endif
    }
    void stop() {
#ifdef __linux__
        for (int i = 0; i < NC; i++) if (fd[i] >= 0) { ioctl(fd[i], PERF_EVENT_IOC_DISABLE, 0); long long v = -1; if (read(fd[i], &v, sizeof v) != sizeof v) v = -1; val[i] = v; }
#endif
    }
};
