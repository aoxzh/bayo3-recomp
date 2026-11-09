// Read the program's executable code into memory at start, in the background, so the first cutscene or fight does
// not stop while the OS pages in recompiled functions from the disk one 4 KiB page at a time. The recompiled game is
// most of the program (about 140 MB on Windows), and much of it runs for the first time well after the title screen.
// One low-priority thread: Windows asks the memory manager for the image's executable sections at once
// (PrefetchVirtualMemory, Windows 8+), every host then touches one byte per page so nothing is left to fault in.
// NX_CODE_PRELOAD=0 turns it off; the log says how much was read and how long it took.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <link.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace nx::plat {
namespace {

struct Range {
    uintptr_t start, size;
};

#if defined(_WIN32)
std::vector<Range> code_ranges() {
    std::vector<Range> out;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return out;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            out.push_back({uintptr_t(base + sec->VirtualAddress), uintptr_t(sec->Misc.VirtualSize)});
    return out;
}

void lower_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE); }

void prefetch(const std::vector<Range>& ranges) {
    struct RangeEntry {  // WIN32_MEMORY_RANGE_ENTRY, which the headers hide below _WIN32_WINNT 0x0602
        PVOID VirtualAddress;
        SIZE_T NumberOfBytes;
    };
    using Fn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, RangeEntry*, ULONG);
    auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory")));
    if (!fn) return;
    std::vector<RangeEntry> e;
    for (auto& r : ranges) e.push_back({reinterpret_cast<PVOID>(r.start), SIZE_T(r.size)});
    fn(GetCurrentProcess(), ULONG_PTR(e.size()), e.data(), 0);
}
#elif defined(__linux__) || defined(__ANDROID__)
// The loaded object that holds this function (the executable, or the game's .so on Android): its executable segments.
std::vector<Range> code_ranges() {
    struct Ctx {
        uintptr_t self;
        std::vector<Range> out;
    } ctx{reinterpret_cast<uintptr_t>(&code_ranges), {}};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* p) -> int {
            auto& c = *static_cast<Ctx*>(p);
            std::vector<Range> mine;
            bool holds = false;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD || !(ph.p_flags & PF_X)) continue;
                const uintptr_t at = info->dlpi_addr + ph.p_vaddr;
                mine.push_back({at, uintptr_t(ph.p_memsz)});
                if (c.self >= at && c.self < at + ph.p_memsz) holds = true;
            }
            if (!holds) return 0;
            c.out = std::move(mine);
            return 1;
        },
        &ctx);
    return ctx.out;
}

void lower_priority() { setpriority(PRIO_PROCESS, 0, 19); }  // this thread only on Linux and Android

void prefetch(const std::vector<Range>& ranges) {
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    for (auto& r : ranges) {
        const uintptr_t a = r.start & ~(page - 1);
        madvise(reinterpret_cast<void*>(a), r.size + (r.start - a), MADV_WILLNEED);
    }
}
#else
std::vector<Range> code_ranges() { return {}; }
void lower_priority() {}
void prefetch(const std::vector<Range>&) {}
#endif

}  // namespace

void lower_thread_priority() { lower_priority(); }

void start_code_preload() {
    if (const char* e = std::getenv("NX_CODE_PRELOAD"); e && std::strcmp(e, "0") == 0) return;
    std::thread([] {
        set_thread_name("code preload");
        lower_priority();
        const uint64_t t0 = monotonic_ns();
        const std::vector<Range> ranges = code_ranges();
        prefetch(ranges);
        uint64_t bytes = 0;
        uint32_t sum = 0;
        for (auto& r : ranges) {
            const volatile uint8_t* p = reinterpret_cast<const volatile uint8_t*>(r.start);
            for (uintptr_t off = 0; off < r.size; off += 4096) sum += p[off];
            bytes += r.size;
        }
        static std::atomic<uint32_t> sink;
        sink = sum;
        log("code preload: %.1f MB of code in memory after %.2f s", double(bytes) / 1e6,
            double(monotonic_ns() - t0) / 1e9);
    }).detach();
}

}  // namespace nx::plat
