// Kernel diagnostics: the hang report (no frame for 5 s, or a dump_threads file in the data folder), the host
// profiler request (a profile file), the optional stall sampler (NX_STALL_SAMPLER/NX_THREAD_DUMP_AT) and guest backtraces.
// Split from kernel.cpp (700-line rule); the state it reads is kernel_state.h.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdarg>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#include <mach/thread_info.h>
#endif
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#include <sys/stat.h>
#if defined(_WIN32)
#include <process.h>
#include <windows.h>  // GetModuleFileNameA (host_exe_path); NOMINMAX/WIN32_LEAN_AND_MEAN come from CMakeLists
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096  // MSVC only has _MAX_PATH
#endif

#include "kernel_state.h"
#include "platform.h"

// kernel_watch.cpp also holds the shared crash report; the last NVN commands come from nvn_debug.cpp. Declared here
// (rather than including nvn_vk.h, which pulls in Vulkan) because this file is compiled for every host.
namespace nx::nvn {
size_t crash_nvn_snapshot(char* out, size_t cap);
}

// NX_CRASH_TEST self-test (crash report paths): a test-only function named like the recompiler's generated code, so a
// fault in it exercises host PC -> generated function -> guest basic block end to end. 0x00670570 is inside main's
// real text range (0x1323400 bytes) and is the real nnMain entry, so nx::describe() resolves a human symbol name.
// Never called unless NX_CRASH_TEST is set (kernel_init starts crash_test_watcher only then).
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
extern "C" void main_00670570(nx::CPU& c);
#endif

namespace nx {

using namespace kstate;

static uint32_t load32(uint64_t a) { return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST); }
// Guest call stack from the AArch64 frame records (x29 -> [prev x29, lr]).
void backtrace(CPU& c) {
    log("  at lr %s", describe(c.x[30]).c_str());
    uint64_t fp = c.x[29];
    for (int k = 0; k < 24 && fp && (fp & 7) == 0; k++) {
        const VmInfo v = vm_query(fp);
        if (v.state == kMemFree) break;
        uint64_t lr = ld<uint64_t>(fp + 8);
        if (!lr) break;
        log("  #%-2d %s  (fp %llx)", k, describe(lr).c_str(), (unsigned long long)fp);
        fp = ld<uint64_t>(fp);
    }
}


// ---- hang report: when frames stop for a while, log what every guest thread is doing --------------------------

static void report_threads() {
    std::vector<std::shared_ptr<KThread>> all;
    {
        std::lock_guard al(g_all_lock);
        for (auto& w : g_all)
            if (auto t = w.lock()) all.push_back(t);
    }
    // The wait lists are read under the kernel lock when it can be had; a thread stuck holding it is itself news.
    std::unique_lock lk(g_k, std::defer_lock);
    for (int k = 0; k < 100 && !lk.try_lock(); k++) plat::sleep_ns(10'000'000);
    if (!lk.owns_lock()) log("hang: kernel lock held by someone, reading without it");
    auto in = [](const std::vector<KThread*>& v, KThread* t) { return std::find(v.begin(), v.end(), t) != v.end(); };
    for (auto& t : all) {
        if (!t->started || t->exited) continue;
        const char* state = "running (guest code or a host call)";
        uint64_t on = 0;
        if (lk.owns_lock() && in(g_mutex_waiters, t.get())) {
            state = "waits for mutex";
            on = t->wait_mutex;
        } else if (lk.owns_lock() && in(g_cv_waiters, t.get())) {
            state = "waits on condvar";
            on = t->wait_key;
        } else if (lk.owns_lock() && in(g_addr_waiters, t.get())) {
            state = "waits on address";
            on = t->wait_addr;
        }
        log("hang: thread %llx (handle %x, entry %s): %s %llx", (unsigned long long)t->id, t->handle,
            describe(t->entry).c_str(), state, (unsigned long long)on);
        if (on && std::strcmp(state, "waits for mutex") == 0)
            log("  mutex word %08x (owner handle %x)", load32(on), load32(on) & ~kHandleWaitMask);
        backtrace(t->ctx);
    }
}

static void watchdog(void*) {
    uint64_t last = 0, since = plat::monotonic_ns();
    bool reported = false;
    for (;;) {
        plat::sleep_ns(1'000'000'000);
        // On request: a file named dump_threads in the data folder (e.g. adb shell touch .../dump_threads). Frames
        // can keep coming while the game's logic is stuck (the render thread is separate).
        std::string req = plat::data_root() + "/dump_threads";
        if (FILE* f = plat::fopen_utf8(req, "rb")) {
            std::fclose(f);
            std::remove(req.c_str());
            log("hang: thread dump requested");
            report_threads();
        }
        // A file named profile (optionally holding a number of seconds): sample every guest thread's host pc into
        // profile.txt next to it.
        req = plat::data_root() + "/profile";
        if (FILE* f = plat::fopen_utf8(req, "rb")) {
            int seconds = 10;
            if (std::fscanf(f, "%d", &seconds) != 1 || seconds <= 0) seconds = 10;
            std::fclose(f);
            std::remove(req.c_str());
            std::vector<int64_t> tids;
            std::vector<const uint64_t*> lrs;
            std::vector<std::shared_ptr<KThread>> keep;  // alive while sampled
            {
                std::lock_guard al(g_all_lock);
                for (auto& w : g_all)
                    if (auto t = w.lock(); t && t->host_tid && !t->exited) {
                        tids.push_back(t->host_tid);
                        lrs.push_back(&t->ctx.x[30]);
                        keep.push_back(t);
                        log("profile: thread %llx is host tid %lld (entry %s)", (unsigned long long)t->id,
                            (long long)t->host_tid, describe(t->entry).c_str());
                    }
            }
            log("profile: sampling %zu threads for %d s", tids.size(), seconds);
            bool ok = plat::profile(tids.data(), lrs.data(), tids.size(), seconds, plat::data_root() + "/profile.txt");
            log("profile: %s", ok ? "written to profile.txt" : "not available on this platform");
        }
        uint64_t f = g_frame, now = plat::monotonic_ns();
        if (f != last || plat::window_minimized()) {
            last = f;
            since = now;
            reported = false;
            continue;
        }
        if (f && !reported && now - since > 5'000'000'000ull) {
            reported = true;
            log("hang: no frame for 5 s after frame %llu; threads (two samples, 1 s apart):", (unsigned long long)f);
            report_threads();
            plat::sleep_ns(1'000'000'000);
            log("hang: second sample");
            report_threads();
        }
    }
}


// NX_STALL_SAMPLER=1: while no frame comes for more than 60 ms, sample where every guest thread is (its lr, every
// 5 ms); a stall over 100 ms is logged with each thread's most frequent place, to tell what the frame waited for.
// NX_THREAD_DUMP_AT=<seconds> also enables this diagnostic thread and requests one frame-pointer walk. Ordinary
// gameplay does not need its periodic wakeups, thread-list locking, map allocations or per-stall logging.
static void dump_threads() {
    std::lock_guard al(g_all_lock);
    for (auto& w : g_all) {
        auto t = w.lock();
        if (!t || !t->started || t->exited) continue;
        std::string chain = describe(t->ctx.x[30]);
        uint64_t fp = t->ctx.x[29];
        for (int k = 0; k < 12 && fp && (fp & 7) == 0 && fp >= kModuleBase; k++) {
            uint64_t lr = ld<uint64_t>(fp + 8);
            if (!lr) break;
            chain += " < " + describe(lr);
            uint64_t next = ld<uint64_t>(fp);
            if (next <= fp) break;
            fp = next;
        }
        log("threads: %llx %s", (unsigned long long)t->id, chain.c_str());
    }
}

static void stall_sampler(void*) {
    static const char* dump_env = std::getenv("NX_THREAD_DUMP_AT");
    uint64_t dump_at = dump_env ? plat::monotonic_ns() + uint64_t(std::atof(dump_env) * 1e9) : 0;
    uint64_t last_frame = 0, since = plat::monotonic_ns();
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> hits;  // (thread id, guest lr) -> samples
    for (;;) {
        plat::sleep_ns(5'000'000);
        uint64_t f = g_frame, now = plat::monotonic_ns();
        if (dump_at && now >= dump_at) dump_threads(), dump_at = 0;
        if (f != last_frame || plat::window_minimized()) {
            uint64_t ms = (now - since) / 1'000'000;
            if (last_frame && ms > 100 && !hits.empty()) {
                log("stall: %llu ms without a frame after frame %llu; where the threads were:", (unsigned long long)ms,
                    (unsigned long long)last_frame);
                std::map<uint64_t, std::pair<uint64_t, uint32_t>> top;  // thread -> (lr, samples)
                std::map<uint64_t, uint32_t> total;
                for (auto& [k, n] : hits) {
                    total[k.first] += n;
                    if (n > top[k.first].second) top[k.first] = {k.second, n};
                }
                for (auto& [id, t] : top)
                    log("  thread %llx: %s (%u of %u samples)", (unsigned long long)id, describe(t.first).c_str(),
                        t.second, total[id]);
            }
            hits.clear();
            last_frame = f;
            since = now;
            continue;
        }
        if (!f || now - since < 60'000'000) continue;
        std::lock_guard al(g_all_lock);
        for (auto& w : g_all)
            if (auto t = w.lock(); t && t->started && !t->exited) hits[{t->id, t->ctx.x[30]}]++;
    }
}

// ---- crash report: a host fault in recompiled code, described in guest terms ---------------------------------------
// platform_mac.cpp / platform_linux.cpp call crash_report() from their signal handlers before re-raising: the host PC
// is mapped through its generated symbol name to the guest basic block, every printed address is classified against
// the fixed guest layout, and the report carries the guest CPU and thread, the guest backtrace, the last NVN commands
// (nvn_debug.cpp) and the automation goal. The same text, plus raw host addresses, is written to
// crash-<pid>-<time>.txt next to nx.log, so one artifact explains a crash without a debugger
// (docs/CRASH-REPORTS.md; tools/symbolize.py resolves it offline).
//
// The markers below are the interface of the report: CRASH:, GUEST:, GUEST-REGS:, GUEST-BACKTRACE:, NVN-LAST:, GOAL:,
// HOST-STACK:. The log gets one compact pass over them, the artifact every line.

namespace {

constexpr size_t kCrashReportCap = 96 * 1024;
char g_crash_text[kCrashReportCap];
size_t g_crash_text_len = 0;
volatile sig_atomic_t g_crash_reporting = 0;  // a fault inside the handler must not recurse

void artifact(const char* fmt, ...) {
    if (g_crash_text_len + 1 >= sizeof g_crash_text) return;
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(g_crash_text + g_crash_text_len, sizeof g_crash_text - g_crash_text_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_text_len += std::min<size_t>(size_t(n), sizeof g_crash_text - g_crash_text_len - 1);
}

bool hex_digit(char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); }
unsigned hex_value(char ch) {
    if (ch <= '9') return unsigned(ch - '0');
    return unsigned((ch | 0x20) - 'a' + 10);  // uppercase hex digits are common in generated names
}

bool host_exe_path(char* out, size_t cap) {
#if defined(_WIN32)
    // GetModuleFileNameA truncates instead of failing: cap is PATH_MAX, so truncation means "no answer".
    const DWORD n = ::GetModuleFileNameA(nullptr, out, DWORD(cap));
    if (n == 0 || n >= DWORD(cap)) {
        out[0] = 0;
        return false;
    }
    return true;
#elif defined(__APPLE__)
    uint32_t size = uint32_t(cap);
    return _NSGetExecutablePath(out, &size) == 0 && out[0];
#else
    ssize_t n = ::readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
#endif
}

// The fixed guest layout (runtime.h), in the order it is checked. A fault address inside one of these is the guest's
// own view of it; anything else is a host address (or not mapped at all).
struct GuestRegion {
    const char* label;
    uint64_t base, size;
};
const GuestRegion kGuestRegions[] = {
    {"module", kModuleBase, kModuleSize},   {"heap", kHeapBase, kHeapMax},     {"stack", kStackBase, kStackSize},
    {"alias", kAliasBase, kAliasSize},      {"kernel", kKernelBase, kKernelSize}, {"thunk", kThunkBase, kThunkSize},
};

const GuestRegion* guest_region(uint64_t address) {
    for (const GuestRegion& r : kGuestRegions)
        if (address - r.base < r.size) return &r;
    return nullptr;
}

// The generated function naming contract (recomp/emit.py): <module>_<8 uppercase hex digits of the guest
// module-relative basic-block address>, e.g. main_00000030(nx::CPU&) -> __Z13main_00000030RN2nx3CPUE.
struct GeneratedModule {
    const char* prefix;
    const char* label;
    uint64_t base;  // relative to NX_BASE
};
#if defined(NX_BAYONETTA3)
// The three B3 modules keep the reference layout's spacing (runtime.h). That spacing is the contract, not NX_BASE:
// the recompiler emits every module base as NX_BASE + offset, so one generated tree serves every host.
constexpr GeneratedModule kGeneratedModules[] = {
    {"subsdk0_", "subsdk0", kSubsdk0Base - kModuleBase},
    {"main_", "main", 0},
    {"sdk_", "sdk", kSdkBase - kModuleBase},
};
#else
constexpr GeneratedModule kGeneratedModules[] = {{"main_", "main", 0}};
#endif

bool parse_generated(const char* name, uint64_t* guest_offset, const char** module, uint64_t* module_base) {
    if (!name) return false;
    for (const GeneratedModule& m : kGeneratedModules) {
        const size_t prefix_len = std::strlen(m.prefix);
        for (const char* p = std::strstr(name, m.prefix); p; p = std::strstr(p + 1, m.prefix)) {
            const char* h = p + prefix_len;
            uint64_t value = 0;
            int digits = 0;
            for (; digits < 8 && hex_digit(*h); ++digits, ++h) value = value * 16 + hex_value(*h);
            if (digits != 8) continue;
            if (hex_digit(*h)) continue;  // a longer run of hex digits is not a basic-block name
            *guest_offset = value;
            *module = m.label;
            *module_base = m.base;
            return true;
        }
    }
    return false;
}

// A host address resolved to its loaded image (and, where the platform offers it, its symbol). Linux dladdr only
// sees .dynsym, so there the generated functions come from <exe>.syms written by tools/symbolize.py --index.
struct HostSym {
    bool has_image = false, has_symbol = false;
    uint64_t image_base = 0, image_offset = 0, function_offset = 0;
    char image[512] = {};
    char name[512] = {};
};

#if defined(__linux__) || defined(__ANDROID__)
struct SidecarSym {
    uint64_t offset;
    uint32_t name;
};
std::vector<SidecarSym> g_sidecar;
std::vector<char> g_sidecar_names;
std::atomic<bool> g_sidecar_loaded{false};

// Read once at startup (kernel_init), never from the handler: the crash path must not allocate.
void load_symbol_sidecar() {
    if (g_sidecar_loaded.exchange(true, std::memory_order_acq_rel)) return;
    char exe[PATH_MAX] = {};
    if (!host_exe_path(exe, sizeof exe)) return;
    std::string path = std::string(exe) + ".syms";
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char* end = nullptr;
        uint64_t offset = std::strtoull(line, &end, 16);
        if (end == line) continue;
        while (*end == ' ' || *end == '\t') ++end;
        char* eol = end + std::strlen(end);
        while (eol > end && (eol[-1] == '\n' || eol[-1] == '\r' || eol[-1] == ' ' || eol[-1] == '\t')) --eol;
        if (eol == end) continue;
        uint32_t at = uint32_t(g_sidecar_names.size());
        g_sidecar_names.insert(g_sidecar_names.end(), end, eol);
        g_sidecar_names.push_back('\0');
        g_sidecar.push_back({offset, at});
    }
    std::fclose(f);
    std::sort(g_sidecar.begin(), g_sidecar.end(),
              [](const SidecarSym& a, const SidecarSym& b) { return a.offset < b.offset; });
    log("crash: symbol sidecar %s: %zu symbols", path.c_str(), g_sidecar.size());
}
#endif

HostSym host_resolve(uint64_t address) {
    HostSym h;
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
    Dl_info d{};
    if (dladdr(reinterpret_cast<void*>(uintptr_t(address)), &d) && d.dli_fbase) {
        h.has_image = true;
        h.image_base = uint64_t(uintptr_t(d.dli_fbase));
        h.image_offset = address - h.image_base;
        if (d.dli_fname) std::snprintf(h.image, sizeof h.image, "%s", d.dli_fname);
        if (d.dli_sname && d.dli_saddr) {
            h.has_symbol = true;
            h.function_offset = address - uint64_t(uintptr_t(d.dli_saddr));
            std::snprintf(h.name, sizeof h.name, "%s", d.dli_sname);
        }
#if defined(__linux__) || defined(__ANDROID__)
        if (!h.has_symbol && !g_sidecar.empty()) {
            auto it = std::upper_bound(g_sidecar.begin(), g_sidecar.end(), h.image_offset,
                                       [](uint64_t v, const SidecarSym& s) { return v < s.offset; });
            if (it != g_sidecar.begin()) {
                --it;
                h.has_symbol = true;
                h.function_offset = h.image_offset - it->offset;
                std::snprintf(h.name, sizeof h.name, "%s", g_sidecar_names.data() + it->name);
            }
        }
#endif
    }
#endif
    return h;
}

const char* image_basename(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// One address, classified for the guest: guest module (with the address - NX_BASE offset and the nearest guest
// symbol), guest heap/stack/alias/kernel/thunk + offset, or host image / host unmapped.
void format_address(char* out, size_t cap, uint64_t address) {
    const GuestRegion* r = guest_region(address);
    if (r) {
        if (std::strcmp(r->label, "module") == 0) {
            std::string symbol = describe(address);
            std::snprintf(out, cap, "guest module (address - NX_BASE = 0x%llx): %s", (unsigned long long)(address - kModuleBase),
                          symbol.c_str());
        } else {
            std::snprintf(out, cap, "guest %s + 0x%llx", r->label, (unsigned long long)(address - r->base));
        }
        return;
    }
    if (!address) {
        std::snprintf(out, cap, "null");
        return;
    }
    HostSym h = host_resolve(address);
    if (h.has_image && h.has_symbol)
        std::snprintf(out, cap, "host %s+0x%llx (%s+0x%llx)", image_basename(h.image),
                      (unsigned long long)h.image_offset, h.name, (unsigned long long)h.function_offset);
    else if (h.has_image)
        std::snprintf(out, cap, "host %s+0x%llx", image_basename(h.image), (unsigned long long)h.image_offset);
    else
        std::snprintf(out, cap, "host unmapped / not in any loaded image or guest region");
}

// The generated basic block behind a host PC, or "none".
void format_generated(char* out, size_t cap, const HostSym& h) {
    uint64_t guest_offset = 0;
    const char* module = nullptr;
    uint64_t module_base = 0;
    if (!h.has_symbol || !parse_generated(h.name, &guest_offset, &module, &module_base)) {
        std::snprintf(out, cap,
                      "generated=none (host PC is not inside a main_/subsdk0_/sdk_ generated function%s)",
                      h.has_symbol ? "" : "; no symbol for the host PC");
        return;
    }
    const uint64_t guest_va = kModuleBase + module_base + guest_offset;
    std::string symbol = describe(guest_va);
    // The "offset - NX_BASE" arithmetic is spelled out: guest module offset + module base = NX_BASE-relative address.
    std::snprintf(out, cap, "generated=%s host_block_offset=0x%llx guest=%s+0x%llx = NX_BASE+0x%llx symbol=%s",
                  h.name, (unsigned long long)h.function_offset, module, (unsigned long long)guest_offset,
                  (unsigned long long)(guest_va - kModuleBase), symbol.c_str());
}

bool guest_readable(uint64_t address) {
    const VmInfo v = vm_query(address);
    return v.state != kMemFree && (v.perm & 1) != 0 && v.state != kMemInaccessible;
}

// The guest call chain (same AArch64 frame records as nx::backtrace above, but bounds-checked against the guest
// memory map: a bogus frame pointer must not fault inside the handler). Written to the artifact and the log.
void report_guest_backtrace(CPU& c) {
    std::string first = "at lr " + describe(c.x[30]);
    artifact("GUEST-BACKTRACE: %s\n", first.c_str());
    log("GUEST-BACKTRACE: %s", first.c_str());
    uint64_t fp = c.x[29];
    for (int k = 0; k < 24 && fp && (fp & 7) == 0; k++) {
        if (!guest_readable(fp) || !guest_readable(fp + 8)) break;
        uint64_t lr = ld<uint64_t>(fp + 8);
        if (!lr) break;
        char buf[512];
        std::snprintf(buf, sizeof buf, "#%-2d %s (fp %llx)", k, describe(lr).c_str(), (unsigned long long)fp);
        artifact("GUEST-BACKTRACE: %s\n", buf);
        log("GUEST-BACKTRACE: %s", buf);
        fp = ld<uint64_t>(fp);
    }
}

// Guest CPU: x0..x30 plus sp/lr (x31/x30), flags and vector registers. The log gets the scalar state; the artifact
// also gets all 32 128-bit vector registers.
void report_guest_regs(CPU* c) {
    if (!c) {
        artifact("GUEST-REGS: none (the faulting host thread is not a guest thread: no nx::CPU context)\n");
        log("GUEST-REGS: none (not a guest thread; no guest CPU context)");
        return;
    }
    for (int k = 0; k < 28; k += 4) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c->x[k],
                      (unsigned long long)c->x[k + 1], (unsigned long long)c->x[k + 2], (unsigned long long)c->x[k + 3]);
        artifact("GUEST-REGS: %s\n", buf);
        log("GUEST-REGS: %s", buf);
    }
    char buf[256];
    std::snprintf(buf, sizeof buf, "x28 %016llx x29 %016llx x30/lr %016llx", (unsigned long long)c->x[28],
                  (unsigned long long)c->x[29], (unsigned long long)c->x[30]);
    artifact("GUEST-REGS: %s\n", buf);
    log("GUEST-REGS: %s", buf);
    const uint64_t nzcv = nx::get_nzcv(*c);
    std::snprintf(buf, sizeof buf, "sp %016llx  nzcv %08llx  fpcr %016llx  fpsr %016llx  (lr %s)",
                  (unsigned long long)c->x[31], (unsigned long long)nzcv, (unsigned long long)c->fpcr,
                  (unsigned long long)c->fpsr, describe(c->x[30]).c_str());
    artifact("GUEST-REGS: %s\n", buf);
    log("GUEST-REGS: %s", buf);
    for (int k = 0; k < 32; k += 2) {
        char v[256];
        std::snprintf(v, sizeof v, "v%-2d %016llx%016llx  v%-2d %016llx%016llx", k,
                      (unsigned long long)c->v[k].u64[1], (unsigned long long)c->v[k].u64[0], k + 1,
                      (unsigned long long)c->v[k + 1].u64[1], (unsigned long long)c->v[k + 1].u64[0]);
        artifact("GUEST-REGS: %s\n", v);  // compact: the log keeps the scalar state only
    }
}

void report_guest_thread(KThread* t) {
    char buf[768];
    if (!t) {
        std::snprintf(buf, sizeof buf,
                      "thread: none (the faulting host thread is not a guest thread: main/render/audio worker)");
    } else {
        std::string entry = describe(t->entry);
        std::snprintf(buf, sizeof buf, "thread id=%llx handle=%x entry=%s host_tid=%lld prio=%d core=%d tls=%llx",
                      (unsigned long long)t->id, t->handle, entry.c_str(), (long long)t->host_tid, t->prio, t->core,
                      (unsigned long long)t->tls);
    }
    artifact("GUEST: %s\n", buf);
    log("GUEST: %s", buf);
}

// Automation stage counters: what the runtime has reached, independent of the crashing thread.
void report_stage() {
    const char* hle = hle_current_name();
    char buf[512];
    std::snprintf(buf, sizeof buf, "stage frames_presented=%llu host_uptime_ns=%llu last_hle_import=%s",
                  (unsigned long long)g_frame.load(std::memory_order_relaxed), (unsigned long long)plat::monotonic_ns(),
                  hle && *hle ? hle : "(none: not inside an HLE import)");
    artifact("GUEST: %s\n", buf);
    log("GUEST: %s", buf);
}

// ---- the automation goal (docs/CRASH-REPORTS.md) --------------------------------------------------------------
// NX_GOAL="scene=battle step=17" (inline text, or @/path with a file) wins; otherwise <data root>/goal is read. The
// file is UTF-8 text up to 2 KiB, one "key=value" per line (keys scene, step, goal, note are suggested; any other
// lines are kept verbatim). The report prints the flattened content, its mtime and age (a stale goal is visible),
// and the "step=" value on its own so automation can grep it.

struct GoalInfo {
    bool found = false;
    bool from_file = false;
    long long mtime = 0;
    char path[PATH_MAX] = {};
    char source[64] = {};
    char content[2048] = {};
    char step[128] = {};
};

void sanitize_goal(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    for (size_t k = 0; src[k] && n + 1 < cap; k++) {
        const unsigned char ch = static_cast<unsigned char>(src[k]);
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            if (n && dst[n - 1] != ' ') dst[n++] = ' ';
        } else if (ch >= 32 && ch < 127) {
            dst[n++] = char(ch);
        } else {
            dst[n++] = '.';
        }
    }
    while (n && dst[n - 1] == ' ') --n;
    dst[n] = 0;
}

void read_goal(GoalInfo& g) {
    std::memset(&g, 0, sizeof g);
    const char* env = std::getenv("NX_GOAL");
    if (env && *env) {
        std::snprintf(g.source, sizeof g.source, "NX_GOAL");
        if (env[0] == '@') {
            std::snprintf(g.path, sizeof g.path, "%s", env + 1);
            std::snprintf(g.source, sizeof g.source, "NX_GOAL(@path)");
        } else {
            std::snprintf(g.content, sizeof g.content, "%s", env);
            sanitize_goal(g.content, sizeof g.content, g.content);
            g.found = true;
            return;
        }
    } else {
        std::snprintf(g.path, sizeof g.path, "%s", (plat::data_root() + "/goal").c_str());
        std::snprintf(g.source, sizeof g.source, "file");
    }
    if (!g.path[0]) return;
    FILE* f = plat::fopen_utf8(g.path, "rb");
    if (!f) return;
    char raw[sizeof g.content] = {};
    const size_t n = std::fread(raw, 1, sizeof raw - 1, f);
    std::fclose(f);
    raw[n] = 0;
    g.found = true;
    g.from_file = true;
    sanitize_goal(g.content, sizeof g.content, raw);
    // "step=N" (or "step: N") on its own line, for automation: printed as its own field.
    for (const char* p = raw; *p; p++) {
        if (p != raw && p[-1] != '\n') continue;
        if (std::strncmp(p, "step=", 5) == 0 || std::strncmp(p, "step: ", 6) == 0) {
            const char* v = p + (p[4] == '=' ? 5 : 6);
            size_t k = 0;
            while (v[k] && v[k] != '\n' && v[k] != '\r' && k + 1 < sizeof g.step) {
                g.step[k] = v[k];
                k++;
            }
            g.step[k] = 0;
            break;
        }
    }
#if !defined(_WIN32)
    struct stat st {};
    if (::stat(g.path, &st) == 0) g.mtime = (long long)st.st_mtime;
#endif
}

void report_goal() {
    GoalInfo g;
    read_goal(g);
    char buf[4096];
    if (!g.found) {
        std::snprintf(buf, sizeof buf,
                      "(none: set NX_GOAL=\"scene=... step=...\" or write %s/goal (key=value lines, <=2 KiB))",
                      plat::data_root().c_str());
    } else {
        long long age = 0;
        if (g.from_file && g.mtime) {
#if defined(_WIN32)
            age = 0;
#else
            age = (long long)std::time(nullptr) - g.mtime;
#endif
        }
        std::snprintf(buf, sizeof buf, "source=%s path=%s mtime=%lld age_s=%lld step=%s content=%s", g.source,
                      g.path[0] ? g.path : "-", g.mtime, age, g.step[0] ? g.step : "-",
                      g.content[0] ? g.content : "(empty)");
    }
    artifact("GOAL: %s\n", buf);
    log("GOAL: %s", buf);
}

// ---- last NVN commands, from the lock-free ring in nvn_debug.cpp -------------------------------------------------
void report_nvn() {
    char ring[16 * 1024];
    const size_t n = nx::nvn::crash_nvn_snapshot(ring, sizeof ring);
    if (!n) {
        artifact("NVN-LAST: (none recorded yet: no command has been replayed)\n");
        log("NVN-LAST: (none recorded yet)");
        return;
    }
    size_t lines = 0;
    for (const char* p = ring; *p; p++)
        if (*p == '\n') lines++;
    char last[4][512] = {};
    size_t last_n = 0, last_pos = 0;
    for (const char* p = ring; *p;) {
        const char* e = std::strchr(p, '\n');
        const size_t len = e ? size_t(e - p) : std::strlen(p);
        const char* text = p;
        size_t text_len = len;
        while (text_len && (*text == ' ' || *text == '\t')) text++, text_len--;
        char line[512];
        std::snprintf(line, sizeof line, "%.*s", int(std::min<size_t>(text_len, sizeof line - 1)), text);
        artifact("NVN-LAST: %s\n", line);
        if (last_n < 4) {
            std::snprintf(last[last_n++], sizeof last[0], "%s", line);
        } else {
            std::snprintf(last[last_pos], sizeof last[0], "%s", line);
            last_pos = (last_pos + 1) % 4;
        }
        if (!e) break;
        p = e + 1;
    }
    // The log gets the newest 4 lines, oldest first (the artifact has all of them).
    for (size_t k = 0; k < last_n; k++) {
        const size_t at = (last_pos + k) % last_n;
        log("NVN-LAST: %s", last[at]);
    }
}

// Opens the self-contained report file next to nx.log (NX_LOG's folder when set; PC: <data root>/.., Android: the data
// root), falling back to the data root and /tmp. It is opened before the report is built so the path can be logged in
// the GUEST section; the caller writes the text and closes it.
FILE* open_crash_artifact(char* path_out, size_t cap) {
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
    char dir[PATH_MAX] = {};
    const char* log_path = std::getenv("NX_LOG");
    if (log_path && *log_path) {
        std::snprintf(dir, sizeof dir, "%s", log_path);
        if (char* slash = std::strrchr(dir, '/'))
            *slash = 0;
        else
            std::snprintf(dir, sizeof dir, ".");
    } else {
#if defined(__ANDROID__)
        std::snprintf(dir, sizeof dir, "%s", plat::data_root().c_str());
#else
        std::snprintf(dir, sizeof dir, "%s/..", plat::data_root().c_str());
#endif
    }
    std::time_t now = std::time(nullptr);
    struct tm tm_buf {};
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char stamp[32] = "unknown-time";
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm_buf);
#if defined(_WIN32)
    const long pid = long(_getpid());
#else
    const long pid = long(getpid());
#endif
    for (int attempt = 0; attempt < 3; attempt++) {
        char folder[PATH_MAX] = {};
        if (attempt == 0)
            std::snprintf(folder, sizeof folder, "%s", dir);
        else if (attempt == 1)
            std::snprintf(folder, sizeof folder, "%s", plat::data_root().c_str());
        else
            std::snprintf(folder, sizeof folder, "/tmp");
        if (!folder[0]) continue;
        std::snprintf(path_out, cap, "%s/crash-%ld-%s.txt", folder, pid, stamp);
        if (FILE* f = plat::fopen_utf8(path_out, "wb")) return f;
    }
    path_out[0] = 0;
    return nullptr;
#else
    (void)path_out;
    (void)cap;
    return nullptr;
#endif
}

const char* signal_label(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
#ifndef _WIN32
        // Windows has no SIGBUS in <signal.h> (the CRT keeps only SIGABRT/FPE/ILL/INT/SEGV/TERM). The Windows crash
        // path goes through platform_win.cpp's unhandled-exception filter, which never calls this.
        case SIGBUS: return "SIGBUS";
#endif
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        default: return "?";
    }
}

}  // namespace

// Called from the platform signal handlers (platform_mac.cpp / platform_linux.cpp): prints the compact CRASH/GUEST/
// GUEST-REGS/GUEST-BACKTRACE/NVN-LAST/GOAL/HOST-STACK sections to the log and writes the full artifact.
void crash_report(int sig, int code, uint64_t host_pc, uint64_t host_lr, uint64_t host_sp, uint64_t host_fp,
                  uint64_t fault_address) {
    if (g_crash_reporting) {
        log("CRASH: signal %d (%s) while the crash report was being written; report abandoned", sig, signal_label(sig));
        return;
    }
    g_crash_reporting = 1;
    g_crash_text_len = 0;

    KThread* t = self_thread();  // thread_local: the crashing thread's guest thread, when it has one
    CPU* c = t ? &t->ctx : nullptr;
    const HostSym pc_sym = host_resolve(host_pc);
    char exe[PATH_MAX] = {};
    host_exe_path(exe, sizeof exe);

    char path[PATH_MAX] = {};
    FILE* file = open_crash_artifact(path, sizeof path);

    // Artifact header (the log gets the marked sections only).
    artifact("Bayonetta3Recomp crash report\n");
    artifact("report-version: 2\n");
    artifact("report file: %s\n", path[0] ? path : "(could not write one)");
    artifact("host executable: %s\n", exe[0] ? exe : "?");
    artifact("nx base: 0x%llx (module layout: main+0x0, subsdk0+0x%llx, sdk+0x%llx)\n",
             (unsigned long long)kModuleBase,
#if defined(NX_BAYONETTA3)
             (unsigned long long)(kSubsdk0Base - kModuleBase), (unsigned long long)(kSdkBase - kModuleBase)
#else
             0ull, 0ull
#endif
    );
    artifact("host pc: 0x%llx\n", (unsigned long long)host_pc);
    artifact("host pc offset (from the image base): 0x%llx\n", (unsigned long long)pc_sym.image_offset);
    artifact("host image base: 0x%llx\n", (unsigned long long)pc_sym.image_base);
    artifact("data address: 0x%llx (signal code %d)\n", (unsigned long long)fault_address, code);

    // CRASH: the one-line summary, same shape as before this report existed.
    {
        char buf[1024];
        std::snprintf(buf, sizeof buf, "CRASH: signal %d (%s) at %s+0x%llx (host pc 0x%llx), data address 0x%llx", sig,
                      signal_label(sig), pc_sym.has_image ? image_basename(pc_sym.image) : "?",
                      (unsigned long long)pc_sym.image_offset, (unsigned long long)host_pc,
                      (unsigned long long)fault_address);
        artifact("%s\n", buf);
        log("%s", buf);
    }

    // GUEST: one contiguous section in the log: the generated block, the classified fault address, the guest thread,
    // the stage counters and the report file.
    {
        char buf[1024];
        format_generated(buf, sizeof buf, pc_sym);
        artifact("GUEST: pc %s\n", buf);
        log("GUEST: pc %s", buf);

        format_address(buf, sizeof buf, fault_address);
        artifact("GUEST: fault 0x%llx class=%s\n", (unsigned long long)fault_address, buf);
        log("GUEST: fault 0x%llx class=%s", (unsigned long long)fault_address, buf);
    }
    report_guest_thread(t);
    report_stage();
    {
        char buf[PATH_MAX + 64];
        std::snprintf(buf, sizeof buf, "report %s", file ? path : "(could not write a crash report file)");
        artifact("GUEST: %s\n", buf);
        log("GUEST: %s", buf);
    }
    report_guest_regs(c);
    if (c) report_guest_backtrace(*c);
    report_nvn();
    report_goal();

    // HOST-STACK: raw host addresses (what resolves the artifact offline) and the other host frames.
    {
        char buf[1024];
        std::snprintf(buf, sizeof buf,
                      "pc=0x%llx image=%s image_base=0x%llx image_offset=0x%llx function_offset=0x%llx symbol=%s",
                      (unsigned long long)host_pc, pc_sym.has_image ? pc_sym.image : "?",
                      (unsigned long long)pc_sym.image_base, (unsigned long long)pc_sym.image_offset,
                      (unsigned long long)pc_sym.function_offset, pc_sym.has_symbol ? pc_sym.name : "(none)");
        artifact("HOST-STACK: %s\n", buf);
        log("HOST-STACK: %s", buf);
        std::snprintf(buf, sizeof buf, "lr=0x%llx sp=0x%llx fp=0x%llx", (unsigned long long)host_lr,
                      (unsigned long long)host_sp, (unsigned long long)host_fp);
        artifact("HOST-STACK: %s\n", buf);
        log("HOST-STACK: %s", buf);
        if (host_lr) {
            format_address(buf, sizeof buf, host_lr);
            artifact("HOST-STACK: lr class=%s\n", buf);
            log("HOST-STACK: lr class=%s", buf);
        }
        artifact("HOST-STACK: recent file opens: see the CRASH section of nx.log (nx::log_recent_files)\n");
    }

    if (file) {
        std::fwrite(g_crash_text, 1, g_crash_text_len, file);
        std::fclose(file);
    }
}

// ---- deliberate-crash self-test (NX_CRASH_TEST=<frames>[:mode]) ------------------------------------------------
// Exists only to exercise the crash report; no other code path reaches it. Unset, empty, 0, off, no, false and
// disabled (case-insensitive) mean "no self-test", so a typo cannot start a deliberate crash. After <frames>
// presented frames it crashes
// in one of three ways:
//   <frames>         a test-only function named like generated code (main_00670570) -> proves host PC -> generated
//                    function -> guest block end to end; the crashing thread is not a guest thread, so the report has
//                    no guest CPU context (that is itself a documented case).
//   <frames>:host    plain host code -> proves the "generated=none" path.
//   <frames>:guest   SIGSEGV sent to the game's main guest thread -> a real nx::CPU context and the real NVN ring
//                    (the frame's commands were replayed on a guest thread).
// 0x00670570 is inside main's real text range (0x1323400 bytes) and is the real nnMain entry, so nx::describe()
// resolves a human symbol (nnMain). Never reached without the environment variable: kernel_init starts the watcher
// only when it is set (zero cost otherwise).
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
// Delivers SIGSEGV to another thread of this process (":guest"), so the handler runs on a real guest thread. macOS
// also picks a thread whose live PC is inside a generated main_/sdk_/subsdk0_ function when there is one: the report
// then shows the whole guest view (generated block, guest CPU, NVN ring) instead of a syscall. False when no guest
// thread can be addressed (the caller falls back to the synthetic crash).
static bool collect_guest_tids(int64_t* tids, size_t max, size_t& count) {
    count = 0;
    std::lock_guard lk(kstate::g_all_lock);
    for (auto& w : kstate::g_all) {
        if (auto th = w.lock(); th && th->started && !th->exited && th->host_tid && count < max)
            tids[count++] = th->host_tid;
    }
    return count > 0;
}

#if defined(__APPLE__)
// A Mach thread's live PC, when it is inside a generated function (dladdr + the generated-name contract).
static bool generated_thread_pc(thread_t th, uint64_t* pc_out, char* function_out, size_t cap) {
    arm_thread_state64_t state{};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(th, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) != KERN_SUCCESS)
        return false;
    const uint64_t pc = arm_thread_state64_get_pc(state);
    Dl_info d{};
    if (!dladdr(reinterpret_cast<void*>(uintptr_t(pc)), &d) || !d.dli_sname) return false;
    uint64_t guest_offset = 0, module_base = 0;
    const char* module = nullptr;
    if (!parse_generated(d.dli_sname, &guest_offset, &module, &module_base)) return false;
    *pc_out = pc;
    std::snprintf(function_out, cap, "%s", d.dli_sname);
    return true;
}
#endif

static bool signal_guest_thread() {
    int64_t tids[256] = {};
    size_t tid_count = 0;
    if (!collect_guest_tids(tids, sizeof tids / sizeof tids[0], tid_count)) return false;
#if defined(__APPLE__)
    pthread_t first = nullptr;
    // Generated code runs for milliseconds at a time every frame, so a short poll finds a guest thread whose live PC
    // is inside a generated function. The thread is suspended first: sending the signal and letting it run would let
    // it leave the generated block before the handler runs (measured: it was inside a syscall by then).
    for (int attempt = 0; attempt < 5000; attempt++) {
        mach_port_t task = mach_task_self();
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(task, &threads, &count) != KERN_SUCCESS) return false;
        mach_port_t found_port = MACH_PORT_NULL;
        pthread_t found_thread = nullptr, first_thread = nullptr;
        uint64_t found_tid = 0, found_pc = 0;
        char found_name[256] = {};
        for (mach_msg_type_number_t k = 0; k < count; k++) {
            thread_identifier_info_data_t info{};
            mach_msg_type_number_t info_count = THREAD_IDENTIFIER_INFO_COUNT;
            if (thread_info(threads[k], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&info), &info_count) !=
                KERN_SUCCESS)
                continue;
            bool guest = false;
            for (size_t j = 0; j < tid_count && !guest; j++) guest = tids[j] == int64_t(info.thread_id);
            if (!guest) continue;
            pthread_t pt = pthread_from_mach_thread_np(threads[k]);
            if (!pt) continue;
            if (!first_thread) first_thread = pt;
            if (found_port == MACH_PORT_NULL &&
                generated_thread_pc(threads[k], &found_pc, found_name, sizeof found_name)) {
                found_port = threads[k];
                found_thread = pt;
                found_tid = info.thread_id;
            }
        }
        bool sent = false, signal_this = false;
        if (found_port != MACH_PORT_NULL) {
            thread_suspend(found_port);  // the PC cannot move while suspended: the handler will see a generated PC
            uint64_t pc_now = 0;
            char name_now[256] = {};
            if (generated_thread_pc(found_port, &pc_now, name_now, sizeof name_now)) {
                signal_this = true;
                log("crash test: SIGSEGV sent to guest thread %llu, executing generated %s at host pc 0x%llx",
                    (unsigned long long)found_tid, name_now, (unsigned long long)pc_now);
                sent = pthread_kill(found_thread, SIGSEGV) == 0;
            }
            thread_resume(found_port);
        }
        for (mach_msg_type_number_t k = 0; k < count; k++) mach_port_deallocate(task, threads[k]);
        vm_deallocate(task, reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
        if (signal_this) return sent;
        if (!first && first_thread) first = first_thread;
        plat::sleep_ns(1'000'000);
    }
    if (!first) return false;
    log("crash test: no guest thread in generated code after 5 s; SIGSEGV sent to the first guest thread");
    return pthread_kill(first, SIGSEGV) == 0;
#else
    // Linux: tgkill addresses one thread of this process by tid (no live-PC inspection: the first guest thread).
    return syscall(SYS_tgkill, getpid(), pid_t(tids[0]), int(SIGSEGV)) == 0;
#endif
}

// NX_CRASH_TEST=<frames>[:host|:guest]. Unset, empty, 0, off, no, false and disabled (case-insensitive) mean
// "no self-test": a typo must not start a deliberate crash. Anything that does not parse as a positive frame
// count is disabled too. Filled once in kernel_init, so the watcher never re-reads the environment.
struct CrashTestConfig {
    unsigned long long frames = 0;
    bool host_mode = false, guest_mode = false;
    bool enabled = false;
};
static CrashTestConfig g_crash_test;

static void crash_test_config_read(CrashTestConfig& cfg) {
    const char* env = std::getenv("NX_CRASH_TEST");
    if (!env || !*env) return;
    const char* colon = std::strchr(env, ':');
    const size_t head = colon ? size_t(colon - env) : std::strlen(env);
    const auto is_word = [&](const char* word) {
        const size_t n = std::strlen(word);
        if (n != head) return false;
        for (size_t k = 0; k < n; k++)
            if (std::tolower(static_cast<unsigned char>(env[k])) != word[k]) return false;
        return true;
    };
    for (const char* word : {"0", "off", "no", "false", "disabled"})
        if (is_word(word)) return;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(env, &end, 10);
    if (end == env || value == 0) return;  // not a number, or "crash after 0 frames"
    cfg.frames = value;
    cfg.enabled = true;
    if (end && *end == ':') {
        cfg.host_mode = std::strncmp(end + 1, "host", 4) == 0;
        cfg.guest_mode = std::strncmp(end + 1, "guest", 5) == 0;
    }
}

static void crash_test_watcher(void* arg) {
    const CrashTestConfig& cfg = *static_cast<const CrashTestConfig*>(arg);
    if (!cfg.enabled) return;
    const unsigned long long frames = cfg.frames;
    const bool host_mode = cfg.host_mode;
    const bool guest_mode = cfg.guest_mode;
    while (g_frame.load(std::memory_order_relaxed) < frames) plat::sleep_ns(10'000'000);
    if (guest_mode) {
        if (signal_guest_thread()) {
            for (;;) plat::sleep_ns(1'000'000'000);  // the SIGSEGV handler on that thread ends the process
        }
        // The guest thread could not be addressed: fall through to the synthetic crash so the path is still probed.
    }
    if (host_mode) {
        *reinterpret_cast<volatile uint64_t*>(uintptr_t(0x10)) = 1;  // plain host fault: no generated symbol
    } else {
        CPU synthetic{};  // not a guest thread: the report says the guest CPU context is this synthetic one
        main_00670570(synthetic);
    }
}
#endif

void kernel_init() {
    plat::start_thread(watchdog, nullptr, 1ull << 20);
    const char* stall_env = std::getenv("NX_STALL_SAMPLER");
    const char* dump_env = std::getenv("NX_THREAD_DUMP_AT");
    const bool sample_stalls = stall_env && *stall_env && std::strcmp(stall_env, "0") != 0;
    if (sample_stalls || (dump_env && *dump_env)) plat::start_thread(stall_sampler, nullptr, 1ull << 20);
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
    crash_test_config_read(g_crash_test);
    if (g_crash_test.enabled) plat::start_thread(crash_test_watcher, &g_crash_test, 1ull << 20);
#if defined(__linux__) || defined(__ANDROID__)
    load_symbol_sidecar();  // before anything can crash: the handler must not allocate
#endif
#endif
}

}  // namespace nx

// NX_CRASH_TEST's crash PC (see above). Global C linkage so the symbol is exactly `main_00670570`, like the
// recompiler's generated functions (which are `__Z13main_00670570RN2nx3CPUE` in the symbol table).
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
extern "C" __attribute__((noinline, used)) void main_00670570(nx::CPU& c) {
    (void)c;
    *reinterpret_cast<volatile uint64_t*>(uintptr_t(0x10)) = 0x5eed;
}
#endif
