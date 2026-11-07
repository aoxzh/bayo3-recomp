#pragma once

#include <cstdlib>
#include "nvn.h"
#include "platform.h"

namespace nx::nvn {
inline bool submit_diag_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_SUBMIT_DIAG");
        return e && e[0] != '0';
    }();
    return on;
}
// Optional eight-present CPU trace. These are host observations, not GPU timestamps.
// Separate from aggregate diagnostics because per-event logging perturbs scheduling.
inline uint64_t submit_trace_from() {
    static const uint64_t from = []() -> uint64_t {
        const char* e = std::getenv("NX_SUBMIT_TRACE");
        return e ? std::strtoull(e, nullptr, 10) : UINT64_MAX;
    }();
    return from;
}
inline bool submit_trace_on() { return submit_trace_from() != UINT64_MAX; }
inline void submit_trace(const char* event, uint64_t id = 0) {
    const uint64_t from = submit_trace_from();
    if (from == UINT64_MAX) return;
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (frame < from || frame - from >= 8) return;
    log("submit-trace: ns %llu tid %lld frame %llu %s %llu",
        (unsigned long long)plat::monotonic_ns(), (long long)plat::current_thread_id(),
        (unsigned long long)frame, event, (unsigned long long)id);
}
}  // namespace nx::nvn
