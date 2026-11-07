// macOS host (Apple silicon): memory (mmap at fixed addresses), pthreads, an SDL3 window with a Metal layer for
// MoltenVK (VK_EXT_metal_surface), SDL3 gamepad + keyboard, SDL3 audio, a signal crash handler and main.
//
// Cocoa owns the main thread: main() starts the game (nx::boot) on its own thread and then runs the SDL event loop.
// Anything that touches the window (creating it, its Metal view) is done there; the game's threads ask for it and
// wait. Input is sampled on the main thread into a snapshot read_pad() copies.
//
// Data: NX_DATA_ROOT or games/<game>'s pc_data_root ("private/<game>"): analysis/{main,sdk}.bin, extract/romfs, user/.
// Settings: NX_<KEY> environment variables, as on Windows.
#include <dlfcn.h>
#include <pthread.h>
#include <cstdlib>
#include <CoreGraphics/CGDisplayConfiguration.h>
#include <mach-o/dyld.h>
#include <mach/mach_vm.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/vm_region.h>
#include <mach/thread_switch.h>
#include <pthread/qos.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "pad_config.h"
#include "platform.h"
#include "render_scale.h"

namespace nx {
void boot();
// kernel_watch.cpp: the guest-first crash report. The platform handler only extracts the host registers.
void crash_report(int sig, int code, uint64_t host_pc, uint64_t host_lr, uint64_t host_sp, uint64_t host_fp,
                  uint64_t fault_address);
}

namespace nx::plat {

namespace {

// Work for the main thread (window creation), run between event polls.
std::mutex g_main_lock;
std::deque<std::function<void()>> g_main_work;

void run_on_main(std::function<void()> fn) {
    if (pthread_main_np()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard lk(g_main_lock);
        g_main_work.push_back([&] {
            fn();
            std::lock_guard l2(m);
            done = true;
            cv.notify_one();
        });
    }
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return done; });
}

SDL_Window* g_window = nullptr;
SDL_MetalView g_view = nullptr;
void* g_layer = nullptr;  // CAMetalLayer*
std::atomic<bool> g_minimized{false};
std::atomic<uint32_t> g_pixel_w{0}, g_pixel_h{0};

std::mutex g_pad_lock;
PadState g_pad;

}  // namespace

// ---- memory ---------------------------------------------------------------------------------------------------

// Diagnostics for a failed reservation: which host mapping is in the way, and with which VM tag (VM_MEMORY_*).
static void dump_host_regions(uint64_t addr, uint64_t size) {
    uint64_t p = addr;
    for (int guard = 0; guard < 64 && p < addr + size; ++guard) {
        mach_vm_address_t at = p;
        mach_vm_size_t sz = 0;
        vm_region_extended_info_data_t info{};
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &at, &sz, VM_REGION_EXTENDED_INFO,
                                          reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS) {
            log("memory:   no host region at %llx (%d)", (unsigned long long)p, kr);
            return;
        }
        log("memory:   host region %llx+%llx prot r%c w%c x%c tag %u resident %u pages", (unsigned long long)at,
            (unsigned long long)sz, info.protection & VM_PROT_READ ? '+' : '-',
            info.protection & VM_PROT_WRITE ? '+' : '-', info.protection & VM_PROT_EXECUTE ? '+' : '-',
            info.user_tag, info.pages_resident);
        if (at + sz <= p) return;  // should not happen
        p = uint64_t(at + sz);
    }
}

// Darwin has no MAP_FIXED_NOREPLACE, and mmap MAP_FIXED *overwrites* whatever is already mapped: that silently
// destroys host mappings (malloc arenas, dylib data, Metal objects) that happen to lie inside the guest's fixed
// window. The symptom was a SIGBUS in libobjc/libsystem_malloc with a data address inside the guest heap, in only
// some processes -- a dangling host pointer whose memory had been replaced by our reservation. A plain hint (no
// MAP_FIXED) is no better: the kernel relocates it at random (measured on macOS 27 / M4: EXACT in only ~11 of 20
// processes, at a different address each time), which no fixed guest layout can tolerate.
//
// A fixed mach_vm_map without VM_FLAGS_OVERWRITE is the primitive that does what is needed: it places the reservation
// atomically at the requested address or fails with KERN_NO_SPACE. Cur protection NONE keeps it uncommitted, max
// protection must be VM_PROT_ALL or commit()'s mprotect is refused (measured).
namespace {
// The guest ranges this process placed itself (reserve_guest_layout, below). memory_init() reserves every range a
// second time; a range we already own is not a conflict, so a repeat of the identical reservation succeeds.
struct GuestRange { uint64_t addr, size; };
GuestRange g_guest_ranges[8];
int g_guest_range_count = 0;
}  // namespace

bool reserve(uint64_t addr, uint64_t size) {
    if ((addr & 0xFFF) || !size || (size & 0xFFF)) return false;
    for (int k = 0; k < g_guest_range_count; ++k)
        if (g_guest_ranges[k].addr == addr && g_guest_ranges[k].size == size) return true;
    mach_vm_address_t a = addr;
    kern_return_t kr = mach_vm_map(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                                   VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY);
    if (kr != KERN_SUCCESS || a != addr) {
        if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, size);  // the kernel never moves a FIXED map
        log("memory: cannot reserve %llx+%llx: %s", (unsigned long long)addr, (unsigned long long)size,
            kr == KERN_SUCCESS ? "the kernel relocated the fixed mapping" : "the range is in use by the host");
        dump_host_regions(0x4000000000ull, 0x40000000000ull);  // the whole upper half: what is there and by whom
        return false;
    }
    if (g_guest_range_count < int(sizeof g_guest_ranges / sizeof g_guest_ranges[0])) {
        g_guest_ranges[g_guest_range_count].addr = addr;
        g_guest_ranges[g_guest_range_count].size = size;
        ++g_guest_range_count;
    }
    return true;
}

// platform.h: the fixed guest layout, placed before SDL/Cocoa can map address space of their own. Measured here: the
// Cocoa/SDL startup reserves an 11.98 GiB PROT_NONE hole at a random address, which landed on the module window
// (0x7bcff00000) in about a third of processes -- mmap MAP_FIXED used to clobber it (SIGBUS in libobjc at startup),
// and a non-clobbering fixed map instead refused to boot. Reserving first means the host maps around us.
bool reserve_guest_layout() {
    bool ok = reserve(kModuleBase, kModuleSize);
    ok = reserve(kHeapBase, kHeapMax) && ok;
    ok = reserve(kAliasBase, kAliasSize) && ok;
    ok = reserve(kStackBase, kStackSize) && ok;
    ok = reserve(kKernelBase, kKernelSize) && ok;
    // The host maps ~12 GiB PROT_NONE holes at ASLR-random addresses before main (measured on macOS 27 / M4: the same
    // hole appears in a program that does nothing), so a fixed 16 GiB layout is hit in roughly a third of processes.
    // Re-running gives a fresh address space; main() does that when this returns false.
    if (ok)
        log("memory: guest layout reserved before host startup (%llx..%llx)", (unsigned long long)kModuleBase,
            (unsigned long long)(kKernelBase + kKernelSize));
    return ok;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize()) - 1;  // 16 KiB on Apple silicon
    uint64_t a = addr & ~page, e = (addr + size + page) & ~page;
    return mprotect(reinterpret_cast<void*>(a), e - a, PROT_READ | PROT_WRITE) == 0;
}

// The guest frees 4 KiB pages; Apple silicon pages are 16 KiB, and mprotect would take the neighbours of a partly
// freed page with it (SIGBUS in Wonder when nn::os's VAMM unmapped single pages). Only whole host pages are
// released; the freed part of a shared one is zeroed, which is what the guest would see if it maps it again.
void decommit(uint64_t addr, uint64_t size) {
    uint64_t page = uint64_t(getpagesize());
    uint64_t lo = (addr + page - 1) & ~(page - 1), hi = (addr + size) & ~(page - 1);
    if (lo >= hi) {
        std::memset(reinterpret_cast<void*>(addr), 0, size);
        return;
    }
    if (addr < lo) std::memset(reinterpret_cast<void*>(addr), 0, lo - addr);
    if (hi < addr + size) std::memset(reinterpret_cast<void*>(hi), 0, addr + size - hi);
    madvise(reinterpret_cast<void*>(lo), hi - lo, MADV_FREE);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_NONE);
}

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static void* thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    // The game's threads (render, jobs, audio) are latency critical: default QoS lets macOS run them on the M1's
    // efficiency cores and stretch their sleeps and wakeups.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    s.fn(s.arg);
    return nullptr;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack_size + 0x3FFF) & ~size_t(0x3FFF));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    pthread_create(&t, &a, thread_entry, new ThreadStart{fn, arg});
    pthread_attr_destroy(&a);
}

void exit_thread() { pthread_exit(nullptr); }
void exit_process(int code) {
    nx::log_flush();  // the logger writes on its own thread
    _exit(code);
}

void sleep_ns(int64_t ns) {
    timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
    nanosleep(&ts, nullptr);
}

// macOS sched_yield drops the caller to priority 0 for a while: measured 20-180 us per call on the M1, and Wonder's
// job system yields after every job item and in its barriers, so the main thread spent ~60% of its frame there and
// the game's governor fell to 30 fps (docs/archive/nvn/DEBUG.md #44). thread_switch with no hint (2-4 us) gives the core to
// another ready thread without that penalty. NX_YIELD=sched: the old call.
void yield() {
    static const bool sched = [] {
        const char* e = std::getenv("NX_YIELD");
        return e && std::strcmp(e, "sched") == 0;
    }();
    if (sched)
        sched_yield();
    else
        thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
}
void set_thread_name(const char* name) { pthread_setname_np(name); }  // Darwin: the calling thread only

int64_t current_thread_id() {
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return int64_t(tid);
}

bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }  // use Instruments

uint64_t monotonic_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
int64_t ftell64(FILE* f) { return ftello(f); }
int fseek64(FILE* f, int64_t off, int origin) { return fseeko(f, off, origin); }

uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    if (size > uint64_t(SSIZE_MAX)) return 0;  // the game passes -1 as "everything": fread read nothing
    int fd = fileno(f);
    uint64_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, static_cast<uint8_t*>(buf) + done, size_t(size - done), off + int64_t(done));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += uint64_t(n);
    }
    return done;
}

bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? r : nx::game::kInfo.pc_data_root;
}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}

// ---- window ---------------------------------------------------------------------------------------------------

bool headless() {
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void* create_window() {
    run_on_main([] {
        if (g_window) return;
        int w = 1280, h = 720;
        // `resolution=WxH`, or `auto` for the largest 16:9 area that fits the display (the same policy as the Linux
        // host: the game sizes its render targets from the window, so the window's shape is the picture's shape).
        std::string r = setting("resolution");
        if (r == "auto") {
            SDL_Rect b{};
            if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &b) && b.w > 0 && b.h > 0) {
                // The largest 16:9 area that fits: the game sizes its render targets from the window, so a stretched
                // window would be a stretched picture (render_scale.h, fit_aspect).
                const nx::plat::RenderSize fit = nx::plat::fit_aspect(uint32_t(b.w), uint32_t(b.h));
                w = int(fit.w), h = int(fit.h);
            }
        } else if (!r.empty() && std::sscanf(r.c_str(), "%dx%d", &w, &h) != 2) {
            w = 1280, h = 720;
        }
        g_window = SDL_CreateWindow(nx::game::kInfo.title, w, h,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL |
                                        (headless() ? SDL_WINDOW_HIDDEN : 0));
        if (!g_window) {
            log_line(SDL_GetError());
            return;
        }
        if (!headless()) SDL_RaiseWindow(g_window);  // without an app bundle it does not come to the front
        g_view = SDL_Metal_CreateView(g_window);
        g_layer = SDL_Metal_GetLayer(g_view);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        g_pixel_w = uint32_t(pw), g_pixel_h = uint32_t(ph);
    });
    return g_layer;  // the Vulkan surface is made from the CAMetalLayer, from any thread
}

void release_window(void*) {}
uint64_t window_generation() { return 1; }
bool window_minimized() { return g_minimized; }

void window_size(uint32_t& w, uint32_t& h) {
    w = g_pixel_w;
    h = g_pixel_h;
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }

// The display's refresh rate, so the vsync clock and the present pacing follow the panel instead of a 60 Hz model
// (hle_vi.cpp display_period_ns). CoreGraphics answers for the window's display; 0 keeps the historical 60 Hz.
const uint8_t* settings_panel(uint32_t, uint32_t&, uint32_t&, uint64_t&) { return nullptr; }  // Windows only
bool settings_panel_open() { return false; }

uint32_t screen_refresh_hz() {
    static const uint32_t hz = [] () -> uint32_t {
        CGDirectDisplayID display = CGMainDisplayID();
        const CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
        if (!mode) return 0;
        const double rate = CGDisplayModeGetRefreshRate(mode);
        CGDisplayModeRelease(mode);
        // A mode with no declared rate (some virtual displays) reports 0: fall back to the historical model.
        const uint32_t rounded = uint32_t(rate + 0.5);
        return rounded >= 24 && rounded <= 480 ? rounded : 0;
    }();
    return hz;
}
float default_render_scale() { return 1.0f; }

// ---- input: player 1's SDL gamepad + keyboard (sampled on the main thread) -------------------------------------

static void sample_input() {
    PadState p;
    // The player's controller settings (pad_config.h): layout, deadzone, trigger, camera inversion and which of the
    // connected controllers is player 1. Read once, so a launcher's switches reach every sample.
    static const nx::plat::PadConfig cfg = nx::plat::pad_config_from_env();
    static SDL_Gamepad* pad = nullptr;
    if (!pad) {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) {
            const int pick = cfg.index < n ? cfg.index : 0;
            pad = SDL_OpenGamepad(ids[pick]);
            if (pad)
                log("input: player 1 is controller %d (%d connected) '%s', %s layout, deadzone %d, trigger %d", pick, n,
                    SDL_GetGamepadName(pad), cfg.swap_face ? "Xbox" : "Nintendo", cfg.deadzone, cfg.trigger);
        }
        SDL_free(ids);
    }
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
    if (pad) {
        // SDL names buttons by position (south/east/...); Nintendo's A is the east button, B the south one.
        struct Map {
            SDL_GamepadButton b;
            uint64_t bit;
        };
        static const Map map[] = {
            {SDL_GAMEPAD_BUTTON_EAST, kA},           {SDL_GAMEPAD_BUTTON_SOUTH, kB},
            {SDL_GAMEPAD_BUTTON_NORTH, kX},          {SDL_GAMEPAD_BUTTON_WEST, kY},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL},  {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
            {SDL_GAMEPAD_BUTTON_START, kPlus},       {SDL_GAMEPAD_BUTTON_BACK, kMinus},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        };
        for (const Map& m : map)
            if (SDL_GetGamepadButton(pad, m.b)) p.buttons |= m.bit;
        if (cfg.swap_face) p.buttons = nx::plat::pad_swap_face(p.buttons);
        if (nx::plat::pad_trigger_pressed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER), cfg.trigger))
            p.buttons |= kZL;
        if (nx::plat::pad_trigger_pressed(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), cfg.trigger))
            p.buttons |= kZR;
        auto axis = [&](SDL_GamepadAxis a, bool invert) {
            const int32_t v = nx::plat::pad_axis(SDL_GetGamepadAxis(pad, a), cfg.deadzone, kStickMax);
            return invert ? -v : v;
        };
        p.lx = axis(SDL_GAMEPAD_AXIS_LEFTX, false);
        p.ly = axis(SDL_GAMEPAD_AXIS_LEFTY, true);  // SDL: down is positive; Horizon: up is positive
        p.rx = axis(SDL_GAMEPAD_AXIS_RIGHTX, false);
        p.ry = axis(SDL_GAMEPAD_AXIS_RIGHTY, true);
        nx::plat::pad_camera(p.rx, p.ry, cfg);
    }
    // Keyboard: WASD / arrows move, Space A, Shift B (run), J X, K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Esc -.
    if (g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS)) {
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]) p.lx = -kStickMax, p.buttons |= kLeft;
        if (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) p.lx = kStickMax, p.buttons |= kRight;
        if (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) p.ly = kStickMax, p.buttons |= kUp;
        if (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]) p.ly = -kStickMax, p.buttons |= kDown;
        if (k[SDL_SCANCODE_SPACE]) p.buttons |= kA;
        if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) p.buttons |= kB;
        if (k[SDL_SCANCODE_J]) p.buttons |= kX;
        if (k[SDL_SCANCODE_K]) p.buttons |= kY;
        if (k[SDL_SCANCODE_Q]) p.buttons |= kL;
        if (k[SDL_SCANCODE_E]) p.buttons |= kR;
        if (k[SDL_SCANCODE_Z]) p.buttons |= kZL;
        if (k[SDL_SCANCODE_C]) p.buttons |= kZR;
        if (k[SDL_SCANCODE_RETURN]) p.buttons |= kPlus;
        if (k[SDL_SCANCODE_ESCAPE]) p.buttons |= kMinus;
    }
    std::lock_guard lk(g_pad_lock);
    g_pad = p;
}

void read_pad(PadState& p) {
    std::lock_guard lk(g_pad_lock);
    p.buttons |= g_pad.buttons;
    if (g_pad.lx) p.lx = g_pad.lx;
    if (g_pad.ly) p.ly = g_pad.ly;
    if (g_pad.rx) p.rx = g_pad.rx;
    if (g_pad.ry) p.ry = g_pad.ry;
    input_script(p);
}

// ---- audio: SDL3 stream, s16 stereo 48 kHz, fed from the queue of submitted buffers ----------------------------

namespace {
struct Pending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};
AudioDone g_done = nullptr;
SDL_AudioStream* g_stream = nullptr;
std::mutex g_audio;
std::deque<Pending> g_pending;

void SDLCALL audio_cb(void*, SDL_AudioStream* s, int additional, int) {
    uint8_t buf[4096];
    while (additional > 0) {
        int n = std::min<int>(additional, sizeof buf);
        int filled = 0;
        std::vector<void*> finished;
        {
            std::lock_guard lk(g_audio);
            while (filled < n && !g_pending.empty()) {
                Pending& p = g_pending.front();
                size_t take = std::min(size_t(n - filled), p.size - p.pos);
                std::memcpy(buf + filled, p.data + p.pos, take);
                filled += int(take);
                p.pos += take;
                if (p.pos == p.size) {
                    finished.push_back(p.ctx);
                    g_pending.pop_front();
                }
            }
        }
        if (filled < n) std::memset(buf + filled, 0, size_t(n - filled));  // underrun: silence
        SDL_PutAudioStreamData(s, buf, n);
        for (void* ctx : finished) g_done(ctx);
        additional -= n;
    }
}
}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    SDL_AudioSpec spec{SDL_AUDIO_S16LE, 2, 48000};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, nullptr);
    if (!g_stream) return false;
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_stream) return false;
    std::lock_guard lk(g_audio);
    g_pending.push_back({static_cast<const uint8_t*>(data), bytes, 0, ctx});
    return true;
}

uint32_t audio_queued() {
    std::lock_guard lk(g_audio);
    return uint32_t(g_pending.size());
}

void audio_pause(bool paused) {
    if (!g_stream) return;
    if (paused)
        SDL_PauseAudioStreamDevice(g_stream);
    else
        SDL_ResumeAudioStreamDevice(g_stream);
}

// ---- Vulkan (MoltenVK through the Vulkan loader SDL finds) ------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    static void* gipa = [] {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            log_line(SDL_GetError());
            return static_cast<void*>(nullptr);
        }
        return reinterpret_cast<void*>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    }();
    return gipa;
}

const char* vulkan_surface_extension() { return VK_EXT_METAL_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateMetalSurfaceEXT"));
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMetalSurfaceCreateInfoEXT ci{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    ci.pLayer = static_cast<const CAMetalLayer*>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler and entry ----------------------------------------------------------------------------------

namespace {

void crash_handler(int sig, siginfo_t* info, void* uc) {
    if ((sig == SIGSEGV || sig == SIGBUS) && nx::plat::write_watch_fault(info->si_addr)) return;  // write_watch.cpp
    const auto* m = static_cast<ucontext_t*>(uc)->uc_mcontext;
    // si_addr is meaningful for SIGSEGV/SIGBUS only; for SIGILL/SIGFPE the faulting address is the PC itself.
    const uint64_t fault = (sig == SIGSEGV || sig == SIGBUS) ? uint64_t(uintptr_t(info->si_addr)) : 0;
    nx::crash_report(sig, info->si_code, m->__ss.__pc, m->__ss.__lr, m->__ss.__sp, m->__ss.__fp, fault);
    nx::log_recent_files();
    signal(sig, SIG_DFL);
    raise(sig);
}

void game_thread() {
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
}

}  // namespace

int main(int argc, char** argv) {
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    // The fixed guest layout, before SDL/Cocoa/Metal take address space: the host reserves multi-GiB holes at random
    // and a fixed layout cannot move out of their way later (platform.h). memory_init() finds these already ours.
    // A collision here is an address-space accident, not a defect: re-exec into a fresh address space (bounded).
    if (!nx::plat::reserve_guest_layout()) {
        const char* e = std::getenv("NX_LAYOUT_ATTEMPT");
        int attempt = e ? std::atoi(e) : 1;
        if (attempt >= 10) {
            std::fprintf(stderr, "cannot place the fixed guest layout after %d address spaces; giving up\n", attempt);
            return 1;
        }
        char exe[PATH_MAX];
        uint32_t size = sizeof exe;
        if (_NSGetExecutablePath(exe, &size) != 0) {
            std::fprintf(stderr, "cannot re-exec: executable path does not fit\n");
            return 1;
        }
        setenv("NX_LAYOUT_ATTEMPT", std::to_string(attempt + 1).c_str(), 1);
        std::fprintf(stderr, "memory: guest layout occupied by the host, restarting in a new address space (attempt %d)\n",
                     attempt + 1);
        execv(exe, argv);
        std::fprintf(stderr, "execv(%s) failed: %s\n", exe, std::strerror(errno));
        return 1;
    }
    unsetenv("NX_LAYOUT_ATTEMPT");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine

    // The game on its own thread, with a big stack (recompiled calls nest deeply); the main thread keeps Cocoa's
    // event loop.
    nx::plat::start_thread([](void*) { game_thread(); nx::plat::exit_process(0); }, nullptr, 64ull << 20);

    using namespace nx::plat;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                nx::log("platform: quit requested (window closed / Cmd-Q)");
                exit_process(0);
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_HIDDEN:
                g_minimized = true;
                break;
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_SHOWN:
                g_minimized = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                g_pixel_w = uint32_t(e.window.data1), g_pixel_h = uint32_t(e.window.data2);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_F11 && g_window)
                    SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
                break;
            default:
                break;
            }
        }
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lk(g_main_lock);
            work.swap(g_main_work);
        }
        for (auto& fn : work) fn();
        sample_input();
        SDL_Delay(2);
    }
}
