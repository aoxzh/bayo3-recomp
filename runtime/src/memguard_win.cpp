// Memory guard (Windows), as memguard_mac.cpp: the process's private commit (PrivateUsage: the guest heap, host
// allocations and what the GPU driver commits for us; with an integrated GPU that includes its "video" memory) is
// checked every 250 ms; past NX_MEM_LIMIT_GB (default 8) the process ends at once, so a leak can never push the
// machine into swap (one reached 85 GB on the Mac). The footprint is logged every 30 s so growth shows early.
// NX_MEMGUARD_STANDALONE: only the helpers, for tests/memguard_test.cpp.
#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifndef NX_MEMGUARD_STANDALONE
#include <nx/runtime.h>

#include "platform.h"
#endif

namespace nx::plat {

// Bytes of private memory committed by this process (0 if the query fails).
uint64_t memory_footprint() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        return 0;
    return uint64_t(pmc.PrivateUsage);
}

// NX_MEM_LIMIT_GB's value in GB: a positive number, else 60% of this machine's RAM (as memguard_android.cpp does),
// falling back to 8 GB if the query fails. A fixed 8 GB is a 16 GB host's default and stops a 1280x720 session that
// reaches the Prologue on a 32 GB machine: `memory: 8.10 GB is over the 8.0 GB limit`.
double memory_limit_gb(const char* env) {
    double gb = env ? std::atof(env) : 0.0;
    if (gb > 0) return gb;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    const double ram = GlobalMemoryStatusEx(&ms) ? double(ms.ullTotalPhys) / double(1ull << 30) : 0.0;
    return ram > 0 ? ram * 0.6 : 8.0;
}

#ifndef NX_MEMGUARD_STANDALONE

void start_memory_guard() {
    const char* env = std::getenv("NX_MEM_LIMIT_GB");
    const double gb = memory_limit_gb(env);
    const uint64_t limit = uint64_t(gb * double(1ull << 30));
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    const double ram = GlobalMemoryStatusEx(&ms) ? double(ms.ullTotalPhys) / double(1ull << 30) : 0.0;
    log("memory: guard on, limit %.1f GB (%s), the machine has %.1f GB", gb,
        env && std::atof(env) > 0 ? "NX_MEM_LIMIT_GB" : "60% of RAM", ram);
    std::thread([limit, gb] {
        uint64_t ticks = 0, peak = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = memory_footprint();
            if (f > peak) peak = f;
            if (++ticks % 120 == 0)
                log("memory: %.2f GB (peak %.2f GB, limit %.1f GB)", double(f) / double(1ull << 30),
                    double(peak) / double(1ull << 30), gb);
            if (f > limit) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "memory: %.2f GB is over the %.1f GB limit (NX_MEM_LIMIT_GB): stopping",
                              double(f) / double(1ull << 30), gb);
                log("%s", msg);
                std::fprintf(stderr, "%s\n", msg);
                log_flush();
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    }).detach();
}

#endif

uint64_t memguard_overlap_ns(uint64_t, uint64_t) { return 0; }

}  // namespace nx::plat
