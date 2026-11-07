// Write watches (docs/archive/nvn/android-perf/PLAN.md R4): whether the CPU wrote a range since it was last asked, from page
// protection instead of hashing. A watched range is mapped read-only; the first write into one of its pages faults,
// the crash handler (platform_android.cpp, platform_mac.cpp) hands the address here first, and the page is made
// writable again with the range marked written, so the store retries and succeeds. write_watch_take re-protects the
// range and then reads the mark: a store before the re-protection found the mark already set (its page was writable
// only because a fault set it), one after it faults again.
//
// For Wonder's glyph cache (a 16 MB pitch-linear texture the CPU writes without a flush), which was hashed in full
// every 4th frame (~5 ms on a phone, a spike every 4 frames). Only for game code's stores: the kernel writing into a
// read-only page fails the syscall instead (EFAULT), so it is kept to textures, and dropped with them. POSIX only;
// elsewhere (and with NX_WRITE_WATCH=0) write_watch_add fails and callers hash as before.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "platform.h"

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace nx::plat {

#if !defined(_WIN32)
namespace {

struct Watch {
    std::atomic<uint64_t> addr{0};  // the caller's address (0: a free slot)
    std::atomic<uint64_t> begin{0}, end{0};  // the pages it covers
    uint64_t size = 0;                       // the caller's size
    std::atomic<bool> written{false};
    // The pages written since the last take, as the first and the end of them (UINT64_MAX / 0: none known). Set before
    // `written`, so a take that sees the flag also sees the range; one that races a fault finds the range alone.
    std::atomic<uint64_t> lo{UINT64_MAX}, hi{0};
};
constexpr int kWatches = 16;
Watch g_watches[kWatches];
// Initialized on the ordinary add path before a watch can fault. No getenv,
// allocation, locks or logging in the signal handler.
std::atomic<bool> g_diag{false};
std::atomic<uint64_t> g_adds{0}, g_takes{0}, g_removes{0}, g_faults{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

uint64_t page_size() {
    static const uint64_t p = uint64_t(sysconf(_SC_PAGESIZE));
    return p;
}

bool enabled() {
    static const bool on = [] {
        const char* e = std::getenv("NX_WRITE_WATCH");
        return !(e && e[0] == '0');
    }();
    return on;
}

}  // namespace

bool write_watch_add(uint64_t addr, uint64_t size) {
    if (!enabled() || !addr || !size) return false;
    static const bool diag = [] {
        const char* e = std::getenv("NX_SUBMIT_DIAG");
        return e && e[0] != '0';
    }();
    g_diag.store(diag, std::memory_order_relaxed);
    if (diag) g_adds.fetch_add(1, std::memory_order_relaxed);
    uint64_t ps = page_size(), b = addr & ~(ps - 1), e = (addr + size + ps - 1) & ~(ps - 1);
    for (Watch& w : g_watches) {
        uint64_t expected = 0;
        if (!w.addr.compare_exchange_strong(expected, addr)) continue;
        w.begin = b, w.end = e, w.size = size, w.written = true;  // unknown until the first take: written
        w.lo = UINT64_MAX, w.hi = 0;
        if (mprotect(reinterpret_cast<void*>(b), e - b, PROT_READ) != 0) {
            w.addr = 0;
            return false;
        }
        return true;
    }
    return false;
}

void write_watch_remove(uint64_t addr) {
    for (Watch& w : g_watches)
        if (w.addr == addr) {
            if (g_diag.load(std::memory_order_relaxed)) g_removes.fetch_add(1, std::memory_order_relaxed);
            mprotect(reinterpret_cast<void*>(w.begin.load()), w.end - w.begin, PROT_READ | PROT_WRITE);
            w.begin = w.end = 0;
            w.addr = 0;
        }
}

bool write_watch_take(uint64_t addr, uint64_t* lo, uint64_t* hi) {
    if (lo) *lo = 0, *hi = UINT64_MAX;  // unknown: all of it
    for (Watch& w : g_watches)
        if (w.addr == addr) {
            if (g_diag.load(std::memory_order_relaxed)) g_takes.fetch_add(1, std::memory_order_relaxed);
            mprotect(reinterpret_cast<void*>(w.begin.load()), w.end - w.begin, PROT_READ);
            bool was = w.written.exchange(false);
            uint64_t a = w.lo.exchange(UINT64_MAX), b = w.hi.exchange(0);
            if (a < b) {  // the written pages, clipped to the caller's range and made relative to it
                a = a > addr ? a - addr : 0;
                b = std::min<uint64_t>(b - addr, w.size);
                if (lo) *lo = a, *hi = std::max(a, b);
            } else if (lo && !was) {
                *lo = *hi = 0;  // nothing written
            }
            return was || a < b;
        }
    return true;
}

bool write_watch_fault(void* at) {
    uint64_t a = reinterpret_cast<uint64_t>(at);
    bool hit = false;
    uint64_t page = a & ~(page_size() - 1);
    for (Watch& w : g_watches)
        if (w.addr && a >= w.begin && a < w.end) {
            for (uint64_t cur = w.lo; page < cur && !w.lo.compare_exchange_weak(cur, page);) {}
            for (uint64_t cur = w.hi; page + page_size() > cur && !w.hi.compare_exchange_weak(cur, page + page_size());) {}
            w.written = true, hit = true;
        }
    if (hit) {
        if (g_diag.load(std::memory_order_relaxed)) g_faults.fetch_add(1, std::memory_order_relaxed);
        mprotect(reinterpret_cast<void*>(a & ~(page_size() - 1)), page_size(), PROT_READ | PROT_WRITE);
    }
    return hit;
}

// NX_LATE_GUARD. No getenv, allocation or logging on the fault path, as above. A slot keeps its range after it is
// opened, until the next add: a thread that faulted just before late_guard_open must still find it.
namespace {
struct Guard {
    std::atomic<uint64_t> begin{0}, end{0};
};
constexpr uint32_t kGuards = 256, kGuardEvents = 32;
Guard g_guards[kGuards];
std::atomic<uint32_t> g_guard_count{0}, g_guard_events{0};
struct GuardRecord {
    std::atomic<uint64_t> addr{0}, pc{0}, tid{0};
    std::atomic<bool> write{false};
} g_guard_records[kGuardEvents];
bool g_guard_fresh = true;  // the next add starts a new set (replay thread only)
}  // namespace

bool late_guard_on() {
    static const bool on = [] {
        const char* e = std::getenv("NX_LATE_GUARD");
        return e && e[0] == '1' && !e[1];
    }();
    return on;
}

uint64_t late_guard_add(uint64_t addr, uint64_t size) {
    const uint64_t ps = page_size(), b = (addr + ps - 1) & ~(ps - 1), e = (addr + size) & ~(ps - 1);
    if (e <= b) return 0;
    if (g_guard_fresh) g_guard_count = 0, g_guard_fresh = false;
    const uint32_t n = g_guard_count.load();
    if (n == kGuards) return 0;
    g_guards[n].begin = b, g_guards[n].end = e;
    g_guard_count = n + 1;
    if (mprotect(reinterpret_cast<void*>(b), e - b, PROT_NONE) != 0) return 0;
    return e - b;
}

uint32_t late_guard_open(LateGuardEvent* out, uint32_t room) {
    const uint32_t n = g_guard_count.load();
    for (uint32_t k = 0; k < n; k++) {
        const uint64_t b = g_guards[k].begin.load();
        mprotect(reinterpret_cast<void*>(b), g_guards[k].end.load() - b, PROT_READ | PROT_WRITE);
    }
    g_guard_fresh = true;
    const uint32_t seen = g_guard_events.exchange(0);
    uint32_t count = 0;
    for (; count < std::min({seen, room, kGuardEvents}); count++) {
        const GuardRecord& r = g_guard_records[count];
        out[count] = {r.addr.load(), r.pc.load(), r.tid.load(), r.write.load()};
    }
    return seen;
}

bool late_guard_fault(void* at, bool write, uint64_t pc) {
    const uint64_t a = reinterpret_cast<uint64_t>(at);
    const uint32_t n = g_guard_count.load();
    for (uint32_t k = 0; k < n; k++) {
        const uint64_t b = g_guards[k].begin.load(), e = g_guards[k].end.load();
        if (a < b || a >= e) continue;
        const uint32_t i = g_guard_events.fetch_add(1);
        if (i < kGuardEvents) {
            GuardRecord& r = g_guard_records[i];
            r.addr = a, r.pc = pc, r.write = write;
            r.tid = uint64_t(gettid());
        }
        mprotect(reinterpret_cast<void*>(b), e - b, PROT_READ | PROT_WRITE);
        return true;
    }
    return false;
}

WriteWatchSnapshot write_watch_snapshot() {
    WriteWatchSnapshot s;
    for (const Watch& w : g_watches) {
        const uint64_t addr = w.addr.load(), b = w.begin.load(), e = w.end.load();
        if (addr && b < e && w.addr.load() == addr) s.ranges[s.count++] = {b, e};
    }
    s.adds = g_adds.exchange(0); s.takes = g_takes.exchange(0);
    s.removes = g_removes.exchange(0); s.faults = g_faults.exchange(0);
    return s;
}
#else
bool write_watch_add(uint64_t, uint64_t) { return false; }
void write_watch_remove(uint64_t) {}
bool write_watch_take(uint64_t, uint64_t* lo, uint64_t* hi) {
    if (lo) *lo = 0, *hi = UINT64_MAX;
    return true;
}
bool write_watch_fault(void*) { return false; }
bool late_guard_on() { return false; }
uint64_t late_guard_add(uint64_t, uint64_t) { return 0; }
uint32_t late_guard_open(LateGuardEvent*, uint32_t) { return 0; }
bool late_guard_fault(void*, bool, uint64_t) { return false; }
WriteWatchSnapshot write_watch_snapshot() { return {}; }
#endif

}  // namespace nx::plat
