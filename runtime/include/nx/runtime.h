// Host runtime: memory map, module loader/linker, dispatch, HLE registry, logging.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <nx/cpu.h>
#include <nx/module.h>

namespace nx {

// ---- logging --------------------------------------------------------------------------------------------------

void log(const char* fmt, ...);
// Writes every line already queued by log() and flushes the log file. The writer thread is asynchronous; call this
// before the process or the program ends (fatal() and plat::exit_process already do), or before a crash artifact is
// collected, so the last lines are not lost.
void log_flush();
[[noreturn]] void fatal(const char* fmt, ...);
extern bool g_trace;  // verbose tracing (NX_TRACE=1)

// ---- address space --------------------------------------------------------------------------------------------
// Guest pointers are host pointers. Everything the guest can see lives in these reserved ranges.

constexpr uint64_t kAddressSpaceBase = 0x0008000000ull;
constexpr uint64_t kAddressSpaceSize = 0x7FF8000000ull;  // 39-bit, like an application on Horizon
constexpr uint64_t kModuleBase = NX_BASE;  // main; sdk at +0x10000000 (NX_BASE, nx/cpu.h)
constexpr uint64_t kModuleSize = 0x20000000;  // shared 512 MiB reservation
#if defined(NX_BAYONETTA3)
// The three modules keep the reference layout's spacing (main +0, subsdk0 +0x0800_0000, sdk +0x1000_0000) relative to
// NX_BASE. That spacing is the contract, not the absolute value: the recompiler emits every module base as
// NX_BASE + offset (recomp/__main__.py) and the analysis images are module-relative, so one generated tree serves
// every host -- the same mechanism that gives the upstream project Windows/Linux/macOS/Android. Pinning the absolute
// address here contradicted it and broke that portability (macOS maps NX_BASE elsewhere, nx/cpu.h).
constexpr uint64_t kSubsdk0Base = kModuleBase + 0x08000000;
constexpr uint64_t kSdkBase = kModuleBase + 0x10000000;
// What still has to hold for the layout to be meaningful: all three bases inside the shared 512 MiB reservation, and
// each of them page aligned (a module is mapped at a page boundary). The bases themselves are checked against the
// generated table at load time (main.cpp), which is the real guard.
static_assert(kSdkBase < kModuleBase + kModuleSize, "the sdk base must lie inside the B3 module reservation");
static_assert(((kModuleBase | kSubsdk0Base | kSdkBase) & 0xFFFull) == 0, "B3 module bases must be page aligned");
#endif
constexpr uint64_t kHeapMax = 0x180000000ull;  // 6 GiB
constexpr uint64_t kStackSize = 0x100000000ull;
constexpr uint64_t kKernelSize = 0x100000000ull;
constexpr uint64_t kThunkSize = 0x100000ull;
#if defined(__APPLE__)
// macOS layout, packed against the TOP of Horizon's 39-bit application space (so every region stays below 0x80_0000_0000,
// which is what svcGetInfo reports as the address space end). Two measurements on macOS 27 / Apple M4 drove this:
//   1. The layout documented for macOS 14 no longer reserves: this OS keeps 0x10_0000_0000..0x70_0000_0000 reserved and
//      a 52 GiB read-only region at 0x2F40_0000..0xFC00_0000, so nothing below 0x70_0000_0000 maps at a fixed address.
//   2. A plain mmap hint (no MAP_FIXED) is not usable for a fixed layout: the kernel relocated it in ~9 of 20 processes,
//      to a different address each time. platform_mac.cpp now uses MAP_FIXED after verifying the hole.
// Each region keeps its normal size (modules 512 MiB, heap 6 GiB, alias 2 GiB, stack 4 GiB, kernel 4 GiB) and the whole
// set reserved EXACTLY in 10 of 10 attempts, each region probed in its own process.
constexpr uint64_t kHeapBase = 0x7beff00000ull;
constexpr uint64_t kAliasBase = 0x7d6ff00000ull;
constexpr uint64_t kAliasSize = 0x80000000ull;
constexpr uint64_t kStackBase = 0x7deff00000ull;
constexpr uint64_t kKernelBase = 0x7eeff00000ull;
constexpr uint64_t kThunkBase = 0x7feff00000ull;
#else
constexpr uint64_t kHeapBase = 0x5000000000ull;
#if defined(__ANDROID__)
// Android maps the zygote's executable (app_process64, position independent) at a random address from about
// 0x55_5555_0000 to 0x65_5555_0000 at every boot, inside 0x58..0x68: a Snapdragon 8 Gen 2 phone refused the alias reservation
// ("the range is in use", app_process64 at 0x5f_0972_b000). Below the modules nothing is mapped (ART keeps to the
// low 4 GiB), so the alias region goes there. The guest learns it from svcGetInfo, like every region but the modules.
constexpr uint64_t kAliasBase = 0x2000000000ull;
#else
constexpr uint64_t kAliasBase = 0x5800000000ull;
#endif
constexpr uint64_t kAliasSize = 0x1000000000ull;
constexpr uint64_t kStackBase = 0x6800000000ull;
// Kept below 0x70_0000_0000: Android (39-bit address space) maps the process's own libraries and heap up there.
constexpr uint64_t kKernelBase = 0x6A00000000ull;
constexpr uint64_t kThunkBase = 0x6F00000000ull;
#endif
// kKernelBase: TLS pages, main thread stack (memory we hand out). kThunkBase: HLE function addresses (never mapped).

void memory_init();
bool commit(uint64_t addr, uint64_t size);   // make [addr, addr+size) read/write
void decommit(uint64_t addr, uint64_t size);
uint64_t kernel_alloc(uint64_t size, uint64_t align = 0x1000);  // from the kernel region

// Horizon memory states for QueryMemory.
enum MemState : uint32_t {
    kMemFree = 0x00,
    kMemCode = 0x03,
    kMemCodeData = 0x04,
    kMemHeap = 0x05,
    kMemStatic = 0x0C,  // module ro/data seen as static
    kMemStack = 0x0B,
    kMemThreadLocal = 0x0C,
    kMemAlias = 0x0E,
    kMemInaccessible = 0x10,
};
void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);
struct VmInfo {
    uint64_t addr, size;
    uint32_t state, perm;
};
VmInfo vm_query(uint64_t addr);

// ---- modules --------------------------------------------------------------------------------------------------

struct LoadedModule {
    const ModuleInfo* info;
    uint64_t base, size;
    Fn* table;  // text offset / 4 -> function
};

void load_modules(const ModuleInfo* const* mods, size_t count, const std::string& data_dir);
uint64_t find_symbol(const char* name);  // 0 when not found
const LoadedModule* module_at(uint64_t addr);
std::string describe(uint64_t addr);  // "sdk+0x1234 (symbol+0x10)"

// ---- HLE ------------------------------------------------------------------------------------------------------

using HleFn = void (*)(CPU&);
struct HleRegistrar {
    HleRegistrar(const char* name, HleFn fn);
};
#define NX_HLE_CAT2(a, b) a##b
#define NX_HLE_CAT(a, b) NX_HLE_CAT2(a, b)
#define NX_HLE(sym, fn) static ::nx::HleRegistrar NX_HLE_CAT(hle_reg_, __LINE__)(sym, fn)

// B3 fails on unsupported imports/relocations/NVN by default. NX_B3_DIAGNOSTIC=1 explicitly permits
// logging fallbacks for investigation only; it is not an implementation of the missing behavior.
bool strict_runtime();
uint64_t hle_address(const char* name);  // thunk address; unknown names fail in strict mode
uint64_t hle_address_or(const char* name, HleFn fallback);  // unknown names get `fallback`
const char* hle_current_name();  // inside an HLE function: the name its thunk was made for
HleFn hle_lookup(const char* name);

// Call guest code at `addr` on context c (args already in x0..).
inline void guest_call(CPU& c, uint64_t addr) { call(c, addr); }

// ---- kernel ---------------------------------------------------------------------------------------------------

extern std::atomic<uint64_t> g_frame;  // frames presented so far (vkQueuePresentKHR)

void kernel_init();
uint32_t create_main_thread(uint64_t stack_size);  // the calling host thread becomes it
CPU& current_cpu();
uint64_t system_tick();
// A file the game can open under a mount (e.g. "Content/Mods/X/a.lua" for Content:/Mods/X/a.lua), served from
// memory; it takes precedence over a real file of the same path (hle_fs.cpp).
void add_virtual_file(const std::string& path, std::string data);
// The contents of a real game file under the data folder ("" if missing).
std::string read_game_file(const std::string& relative_to_romfs);
// The last file and directory opens of every thread (a small ring, always on), logged by the crash handlers: what
// the game was loading when it crashed (hle_fs.cpp).
void log_recent_files();

}  // namespace nx
