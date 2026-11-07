// Module loader and dynamic linker (what rtld does on Horizon), indirect-call dispatch and HLE thunks.
#include <algorithm>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

bool g_trace = false;
std::atomic<uint64_t> g_frame{0};

namespace {

// Logging never touches the disk on the calling thread. log() formats the line and appends it to this queue; one
// background thread opens the file and writes/flushes. MEASURED (Windows, 2026-11-06): the synchronous version cost
// 5-13 ms per line on this machine (antivirus/file filter), and the frame logger emits a dozen lines every 60
// presents, which showed up as a 60-230 ms hitch every second. Producers never wait for the writer: if the disk is
// slower than the log volume, lines are dropped (with one "dropped" notice) rather than delaying a frame.
constexpr size_t kLogQueueMaxBytes = 32u << 20;

struct LogState {
    std::mutex lock;
    std::condition_variable ready;  // producer -> writer
    std::condition_variable idle;   // writer -> log_flush()
    std::deque<std::string> lines;
    size_t bytes = 0;
    uint64_t dropped = 0;
    bool started = false;
    bool writing = false;  // the writer owns the file while this is true
    bool stderr_on = false;
    bool file_tried = false;
    FILE* file = nullptr;
};

LogState& log_state() {
    // Never destroyed: log() may run from static destructors after main returns, and the writer must stay alive
    // until the process actually ends.
    static LogState* state = new LogState;
    return *state;
}

bool log_env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

void log_open_file(LogState& s) {  // the writer thread, with s.writing set
    if (s.file_tried) return;
    s.file_tried = true;
    const char* path = std::getenv("NX_LOG");
#if defined(__ANDROID__)
    std::string def = plat::data_root() + "/nx.log";  // next to the data, readable from the phone
#else
    std::string def = plat::data_root() + "/../nx.log";
#endif
    std::string file = path ? path : def;
    // Keep the previous session's log (a problem report should survive reopening the game).
    std::remove((file + ".prev").c_str());
    std::rename(file.c_str(), (file + ".prev").c_str());
    s.file = std::fopen(file.c_str(), "w");
}

void log_write(LogState& s, const char* line) {
    if (s.stderr_on) plat::log_line(line);
    if (s.file) {
        std::fprintf(s.file, "%s\n", line);
        std::fflush(s.file);
    }
}

void log_writer() {
    LogState& s = log_state();
    for (;;) {
        std::string line;
        uint64_t dropped = 0;
        {
            std::unique_lock lk(s.lock);
            s.ready.wait(lk, [&] { return !s.lines.empty(); });
            line = std::move(s.lines.front());
            s.lines.pop_front();
            s.bytes -= line.size();
            if (s.lines.empty()) dropped = s.dropped, s.dropped = 0;
            s.writing = true;
        }
        log_open_file(s);
        log_write(s, line.c_str());
        if (dropped) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "log: dropped %llu lines (the writer cannot keep up with the disk)",
                          (unsigned long long)dropped);
            log_write(s, buf);
        }
        {
            std::lock_guard lk(s.lock);
            s.writing = false;
            if (s.lines.empty()) s.idle.notify_all();
        }
    }
}

// may_drop false for a FATAL line: it must reach the file even when the queue is over its cap.
void log_append(const char* buf, bool may_drop) {
    LogState& s = log_state();
    {
        std::lock_guard lk(s.lock);
        if (!s.started) {
            s.started = true;
            s.stderr_on = log_env_on("NX_LOG_STDERR");
            std::atexit(log_flush);
            std::thread(log_writer).detach();
        }
        const size_t size = std::strlen(buf);
        if (may_drop && s.bytes + size > kLogQueueMaxBytes) {
            ++s.dropped;
            return;
        }
        s.lines.emplace_back(buf, size);
        s.bytes += size;
    }
    s.ready.notify_one();
}

std::vector<LoadedModule> g_modules;
std::unordered_map<std::string, uint64_t> g_exports;

struct Thunk {
    std::string name;
    HleFn fn;
};
std::vector<Thunk> g_thunks;
std::unordered_map<std::string, uint64_t> g_thunk_by_name;

std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}

thread_local const char* t_missing_name = nullptr;

}  // namespace

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_append(buf, true);
}

void log_flush() {
    LogState& s = log_state();
    std::unique_lock lk(s.lock);
    if (!s.started) return;
    s.idle.wait(lk, [&] { return s.lines.empty() && !s.writing; });
    if (s.file) std::fflush(s.file);
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char line[2080];
    std::snprintf(line, sizeof line, "FATAL: %s", buf);
    log_append(line, false);  // never dropped, even when a slow disk has filled the queue
    log_flush();
    plat::exit_process(1);
}

// ---- HLE registry ---------------------------------------------------------------------------------------------

HleRegistrar::HleRegistrar(const char* name, HleFn fn) { registry()[name] = fn; }

HleFn hle_lookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

bool strict_runtime() {
#if defined(NX_BAYONETTA3)
    static const bool strict = [] {
        const char* diagnostic = std::getenv("NX_B3_DIAGNOSTIC");
        return !diagnostic || std::strcmp(diagnostic, "1") != 0;
    }();
    return strict;
#else
    return false;
#endif
}

static void missing_stub(CPU& c) {
    // Name comes from the thunk that dispatched here.
    if (strict_runtime()) fatal("missing import called: %s (lr %s)", t_missing_name, describe(c.x[30]).c_str());
    log("missing import called: %s (lr %s) -> 0", t_missing_name, describe(c.x[30]).c_str());
    c.x[0] = 0;
}

uint64_t hle_address_or(const char* name, HleFn fallback) {
    static std::mutex lock;  // NVN hands out procs by name at run time, from any thread
    std::lock_guard lk(lock);
    // Other threads index g_thunks while we add to it: never reallocate (one entry per 16 bytes of the region).
    if (g_thunks.capacity() < kThunkSize / 16) g_thunks.reserve(kThunkSize / 16);
    auto it = g_thunk_by_name.find(name);
    if (it != g_thunk_by_name.end()) return it->second;
    if (g_thunks.size() == kThunkSize / 16) fatal("out of HLE thunks");
    HleFn fn = hle_lookup(name);
    uint64_t addr = kThunkBase + g_thunks.size() * 16;
    g_thunks.push_back({name, fn ? fn : fallback});
    g_thunk_by_name[name] = addr;
    return addr;
}

uint64_t hle_address(const char* name) {
    if (strict_runtime() && !hle_lookup(name)) fatal("unresolved HLE import: %s", name);
    return hle_address_or(name, missing_stub);
}

const char* hle_current_name() { return t_missing_name; }

// ---- dispatch -------------------------------------------------------------------------------------------------

const LoadedModule* module_at(uint64_t addr) {
    for (auto& m : g_modules)
        if (addr - m.base < m.size) return &m;
    return nullptr;
}

std::string describe(uint64_t addr) {
    char buf[512];
    if (addr - kThunkBase < kThunkSize) {
        size_t i = (addr - kThunkBase) / 16;
        snprintf(buf, sizeof buf, "hle:%s", i < g_thunks.size() ? g_thunks[i].name.c_str() : "?");
        return buf;
    }
    const LoadedModule* m = module_at(addr);
    if (!m) {
        snprintf(buf, sizeof buf, "%llx", (unsigned long long)addr);
        return buf;
    }
    uint64_t off = addr - m->base;
    // Nearest preceding function symbol.
    const Symbol* best = nullptr;
    for (size_t k = 0; k < m->info->symbol_count; k++) {
        const Symbol& s = m->info->symbols[k];
        if (s.imported || s.type != 2 || s.value > off) continue;
        if (!best || s.value > best->value) best = &s;
    }
    if (best && off - best->value < 0x10000)
        snprintf(buf, sizeof buf, "%s+%llx (%s+%llx)", m->info->name, (unsigned long long)off, best->name,
                 (unsigned long long)(off - best->value));
    else
        snprintf(buf, sizeof buf, "%s+%llx", m->info->name, (unsigned long long)off);
    return buf;
}

void backtrace(CPU& c);

// Indirect calls (every blr of the game) look the target up here: the modules' text ranges in a small flat array.
struct FastModule {
    uint64_t base, text_size;
    const Fn* table;
};
FastModule g_fast[4];
size_t g_fast_count = 0;

void call(CPU& c, uint64_t t) {
    for (size_t k = 0; k < g_fast_count; k++) {
        const FastModule& m = g_fast[k];
        uint64_t off = t - m.base;
        if (off < m.text_size) [[likely]] {
            Fn fn = m.table[off >> 2];
            if (!fn) [[unlikely]] {
                for (int k = 0; k < 31; k += 4)
                    log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k],
                        (unsigned long long)c.x[k + 1], (unsigned long long)c.x[k + 2],
                        (unsigned long long)(k + 3 < 31 ? c.x[k + 3] : c.x[31]));
                backtrace(c);
                fatal("call into the middle of a function: %s (lr %s)", describe(t).c_str(), describe(c.x[30]).c_str());
            }
            fn(c);
            return;
        }
    }
    uint64_t off = t - kThunkBase;
    if (off < kThunkSize && off / 16 < g_thunks.size()) {
        const Thunk& th = g_thunks[off / 16];
        if (g_trace) log("hle %s", th.name.c_str());
        const char* prev = t_missing_name;
        t_missing_name = th.name.c_str();
        th.fn(c);
        t_missing_name = prev;
        return;
    }
    fatal("call to %llx (%s), lr %s", (unsigned long long)t, describe(t).c_str(), describe(c.x[30]).c_str());
}

void trace_sp(CPU& c, uint64_t pc) {
    log("sp %llx x19 %llx at %s", (unsigned long long)c.x[31], (unsigned long long)c.x[19], describe(pc).c_str());
}

[[noreturn]] void unimpl(CPU& c, uint64_t pc, const char* what) {
    for (int k = 0; k < 31; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)(k + 3 < 32 ? c.x[k + 3] : 0));
    backtrace(c);
    fatal("unimplemented instruction at %s: %s (lr %s)", describe(pc).c_str(), what, describe(c.x[30]).c_str());
}

// ---- loading and linking --------------------------------------------------------------------------------------

uint64_t find_symbol(const char* name) {
    auto it = g_exports.find(name);
    return it == g_exports.end() ? 0 : it->second;
}

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir) {
    if (!g_modules.empty() || count > 4) fatal("invalid module load count or repeated load");
    // Validate all ranges before mapping: the three B3 images share one 512 MiB reservation.
    for (size_t i = 0; i < count; i++) {
        const ModuleInfo& m = *mods[i];
        auto fits = [](uint64_t offset, uint64_t size) {
            return offset <= kModuleSize && size <= kModuleSize - offset;
        };
        if (!fits(m.text, m.text_size) || !fits(m.ro, m.ro_size) ||
            !fits(m.data, m.data_size) || !fits(m.bss, m.bss_size))
            fatal("%s: segment exceeds module reservation", m.name);
        uint64_t end = std::max({m.text + m.text_size, m.ro + m.ro_size,
                                 m.data + m.data_size, m.bss + m.bss_size});
        uint64_t size = (end + 0xfff) & ~0xfffull;
        if (m.text != 0 || !m.text_size || (m.text_size & 3) || (m.base & 0xfff) ||
            m.base < kModuleBase || m.base - kModuleBase > kModuleSize ||
            size > kModuleSize - (m.base - kModuleBase) || m.bss < m.data + m.data_size)
            fatal("%s: invalid module layout or base", m.name);
        for (size_t j = 0; j < i; j++) {
            const ModuleInfo& p = *mods[j];
            uint64_t pend = p.base + ((std::max({p.text + p.text_size, p.ro + p.ro_size,
                p.data + p.data_size, p.bss + p.bss_size}) + 0xfff) & ~0xfffull);
            if (m.base < pend && p.base < m.base + size) fatal("modules %s and %s overlap", p.name, m.name);
        }
        for (size_t k = 0; k < m.func_count; k++)
            if ((m.funcs[k].offset & 3) || m.funcs[k].offset >= m.text_size || !m.funcs[k].fn)
                fatal("%s: invalid function table entry", m.name);
        for (size_t k = 0; k < m.init_count; k++)
            if ((m.init_array[k] & 3) || m.init_array[k] >= m.text_size)
                fatal("%s: initializer outside text", m.name);
    }
    // Map every module first so symbols of all of them are known before relocating.
    for (size_t i = 0; i < count; i++) {
        const ModuleInfo* mi = mods[i];
        uint64_t image = mi->data + mi->data_size;
        uint64_t size = (std::max({mi->text + mi->text_size, mi->ro + mi->ro_size,
            mi->data + mi->data_size, mi->bss + mi->bss_size}) + 0xFFF) & ~0xFFFull;
        if (!commit(mi->base, size)) fatal("cannot commit module %s", mi->name);
        std::string path = data_dir + "/" + mi->name + ".bin";
        std::ifstream f(path, std::ios::binary);
        if (!f) fatal("missing %s (run tools/nso2bin.py)", path.c_str());
        f.read(reinterpret_cast<char*>(mi->base), std::streamsize(image));
        if (uint64_t(f.gcount()) != image) fatal("%s: expected %llx bytes", path.c_str(), (unsigned long long)image);
        vm_set(mi->base + mi->text, mi->text_size, kMemCode, 5);
        vm_set(mi->base + mi->ro, mi->ro_size, kMemCode, 1);
        vm_set(mi->base + mi->data, size - mi->data, kMemCodeData, 3);

        LoadedModule lm{mi, mi->base, size, new Fn[mi->text_size / 4]()};
        for (size_t k = 0; k < mi->func_count; k++) lm.table[mi->funcs[k].offset / 4] = mi->funcs[k].fn;
        g_modules.push_back(lm);
        if (g_fast_count < 4) g_fast[g_fast_count++] = {lm.base, mi->text_size, lm.table};

        for (size_t k = 0; k < mi->symbol_count; k++) {
            const Symbol& s = mi->symbols[k];
            if (s.imported || !s.name[0] || s.bind == 0) continue;  // skip locals
            auto it = g_exports.find(s.name);
            if (it == g_exports.end() || s.bind == 1)  // global beats weak; first global wins
                if (it == g_exports.end()) g_exports[s.name] = mi->base + s.value;
        }
        log("loaded %s at %llx (%llu functions, %llu relocs)", mi->name, (unsigned long long)mi->base,
            (unsigned long long)mi->func_count, (unsigned long long)mi->reloc_count);
    }

    size_t hle = 0, missing = 0, linked = 0;
    for (auto& lm : g_modules) {
        const ModuleInfo* mi = lm.info;
        for (size_t k = 0; k < mi->reloc_count; k++) {
            const Reloc& r = mi->relocs[k];
            if (r.offset > lm.size || lm.size - r.offset < sizeof(uint64_t))
                fatal("%s: relocation target outside module at %llx", mi->name, (unsigned long long)r.offset);
            uint64_t* slot = reinterpret_cast<uint64_t*>(lm.base + r.offset);
            if (r.type == R_AARCH64_RELATIVE) {
                *slot = lm.base + uint64_t(r.addend);
                continue;
            }
            if (r.type != R_AARCH64_GLOB_DAT && r.type != R_AARCH64_JUMP_SLOT && r.type != R_AARCH64_ABS64) {
                if (strict_runtime()) fatal("%s: unsupported relocation type %u at %llx", mi->name, r.type,
                                            (unsigned long long)r.offset);
                log("%s: relocation type %u at %llx not handled", mi->name, r.type, (unsigned long long)r.offset);
                continue;
            }
            if (r.sym >= mi->symbol_count) fatal("%s: relocation symbol index %u out of range", mi->name, r.sym);
            const Symbol& s = mi->symbols[r.sym];
            uint64_t target = 0;
            if (s.type == 2 || s.imported) {
                if (hle_lookup(s.name)) {
                    target = hle_address(s.name);
                    hle++;
                }
            }
            if (!target) {
                target = find_symbol(s.name);
                if (target) linked++;
            }
            if (!target && !s.imported) target = lm.base + s.value;
            // ELF undefined weak symbols resolve to zero, including optional linker boundary symbols.
            // They are not callable placeholder imports (DT_INIT consumes several of them as data).
            if (!target && s.imported && s.bind == 2) {
                *slot = uint64_t(r.addend);
                continue;
            }
            if (!target) {
                if (strict_runtime()) fatal("%s: unresolved import '%s' (type %u, bind %u, relocation %llx)",
                    mi->name, s.name, unsigned(s.type), unsigned(s.bind), (unsigned long long)r.offset);
                // Diagnostic/legacy fallback: functions get a logging stub; data stays null.
                log("%s: unresolved import '%s' at %llx (placeholder)", mi->name, s.name, (unsigned long long)r.offset);
                if (s.type == 2 || s.type == 0) target = hle_address(s.name);
                missing++;
            }
            *slot = target + uint64_t(r.addend);
        }
    }
    log("linked: %zu to HLE, %zu across modules, %zu unresolved", hle, linked, missing);
}

}  // namespace nx
