// What the runtime needs from the host OS. platform_win.cpp and platform_android.cpp implement it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nx::plat {

// ---- memory: fixed-address reservations (guest pointers are host pointers) ----
bool reserve(uint64_t addr, uint64_t size);  // exactly at addr, no access
bool commit(uint64_t addr, uint64_t size);   // read/write
void decommit(uint64_t addr, uint64_t size);
// macOS: place the whole fixed guest layout before any host library maps address space of its own. The host reserves
// multi-GiB holes at random (measured: an 11.98 GiB PROT_NONE reservation created by the Cocoa/SDL startup landed on
// the module window in ~1/3 of processes), and a fixed layout cannot be moved afterwards. Other platforms reserve in
// memory_init() (their MAP_FIXED_NOREPLACE does not clobber and the host does not take these ranges); this is a no-op
// where it is not needed.
bool reserve_guest_layout();  // false: the host already maps something in the fixed window (macOS re-execs)

// ---- threads and time ----
using ThreadFn = void (*)(void*);
void start_thread(ThreadFn fn, void* arg, size_t stack_size);
[[noreturn]] void exit_thread();
[[noreturn]] void exit_process(int code);
void sleep_ns(int64_t ns);
void yield();
void set_thread_name(const char* name);
int64_t current_thread_id();  // the host OS thread id
// Samples the given threads (thread CPU time, 1 kHz) for `seconds`: host pc, a few host frames and the guest lr
// (read through guest_lr[k], the thread's x30). Writes one line per sample to `path`:
//   <tid> <guest lr> <module>:<offset> [<module>:<offset> ...]   (host pc first, then callers)
// False where not implemented.
bool profile(const int64_t* tids, const uint64_t* const* guest_lr, size_t count, int seconds, const std::string& path);  // the host thread's name (debuggers, profilers, top)
uint64_t monotonic_ns();

// ---- files ----
FILE* fopen_utf8(const std::string& path, const char* mode);
int64_t ftell64(FILE* f);
int fseek64(FILE* f, int64_t off, int origin);
// Positional read, safe with other threads reading the same FILE (no shared file position). Bytes read.
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size);
bool make_dirs(const std::string& path);

// ---- output ----
void log_line(const char* line);  // stderr / logcat
std::string data_root();
std::string setting(const std::string& key);  // a player setting: config.txt (Android), NX_<KEY> (PC); "" if unset          // where private/ lives ("private" on PC, the app's external files dir on Android)

// ---- movie:: vtable patch (runtime/src/hle_movie_api.cpp; only compiled with NX_MOVIE_PROBE/NX_MOVIE_FFMPEG) ------
// Rewrites the movie::NnCodec / movie::Extractor vtables in the loaded image so virtual calls reach host code
// (docs/archive/LINUX-VERIFICATION-2026-10-22.md §12). Call once, after load_modules(). No-op unless NX_MOVIE_VTABLE is set.
void movie_vtable_init();

// ---- display, input, audio ----
void* create_window();       // native window handle for the Vulkan surface (HWND / ANativeWindow*), kept alive
void release_window(void* w);  // done with a handle from create_window
uint64_t window_generation();  // changes when the OS replaced the window (Android: app back from the background)
bool window_minimized();     // no surface to draw to (minimized / app in background)
void window_size(uint32_t& w, uint32_t& h);  // client size in pixels
void screen_size(uint32_t& w, uint32_t& h);  // the display's own size, landscape (Android; the window on PC)
// The display's refresh rate in Hz, or 0 when the host cannot tell. The host vsync clock and the present pacing use
// it, so a 120 Hz panel is paced at 120 instead of a hardcoded 60 (hle_vi.cpp display_period_ns). 0 keeps the
// historical 60 Hz model.
uint32_t screen_refresh_hz();
// The in-game settings panel (F1; Windows only, nullptr elsewhere): while it is open, its picture (RGBA8, w x h, sized
// for a target `target_h` pixels high) and a version that changes whenever the pixels do; nullptr while closed. The
// present step lays it over the frame (nvn_vk.cpp).
const uint8_t* settings_panel(uint32_t target_h, uint32_t& w, uint32_t& h, uint64_t& version);
bool settings_panel_open();  // the game is held at its present while it is (nvn_queue.cpp)
// The render scale to use when the player sets none: 1 on fast GPUs, less on slower ones (Android).
float default_render_scale();

// NpadButton bits (Nintendo layout: A is the right face button, B the bottom one).
enum : uint64_t {
    kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3, kStickL = 1ull << 4, kStickR = 1ull << 5,
    kL = 1ull << 6, kR = 1ull << 7, kZL = 1ull << 8, kZR = 1ull << 9, kPlus = 1ull << 10, kMinus = 1ull << 11,
    kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14, kDown = 1ull << 15,
    kStickLLeft = 1ull << 16, kStickLUp = 1ull << 17, kStickLRight = 1ull << 18, kStickLDown = 1ull << 19,
    kStickRLeft = 1ull << 20, kStickRUp = 1ull << 21, kStickRRight = 1ull << 22, kStickRDown = 1ull << 23,
};
constexpr int32_t kStickMax = 0x7FFF;

struct PadState {
    uint64_t buttons = 0;  // NpadButton bits
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};
void read_pad(PadState& p);  // player 1
// Touch controls to draw over the picture (touch_layout.cpp, platform_android_input.cpp): sprites from the touch art
// atlas (touch_art.h) centred at x, y (0..1 across the landscape screen), half-width hw and half-height hh (fractions
// of its height), with an opacity and a brightness (1 = the art's own colours). Drawn in order. 0 where nothing is.
struct OverlaySprite {
    float x, y, hw, hh;
    int art;  // touch_art.h art::Id
    float alpha = 1.0f, bright = 1.0f;
};
size_t touch_sprites(OverlaySprite* out, size_t max);
// Performance hints (perf_hint.cpp; Android ADPF, nothing elsewhere): the calling thread takes part in making frames;
// a frame's work took `ns`.
void perf_hint_thread();
void perf_hint_frame(int64_t ns);
void gpu_turbo();  // NX_GPU_TURBO=1: the Adreno GPU at its highest clock (perf_hint.cpp; Android)
// Frame threads and present intervals (frame_stats.cpp): the cores they run on, their CPU time, NX_AFFINITY.
// role 0: the thread that replays and presents (frame_presented ticks it); 1: the game's main thread.
void frame_thread_tick(int role);
void frame_presented();  // after each present to the display
// Linux: asks power-profiles-daemon for its performance profile for as long as this process lives (frame_stats.cpp).
void request_performance_profile();
// Write watches (write_watch.cpp; page protection, POSIX): add fails when unsupported or full; take re-arms and
// says whether the range was written since the last take; the crash handler calls fault first (true: handled).
bool write_watch_add(uint64_t addr, uint64_t size);
void write_watch_remove(uint64_t addr);
// lo/hi (optional): the written bytes as [lo, hi) from addr, page-rounded; [0, UINT64_MAX) when unknown, empty when none.
bool write_watch_take(uint64_t addr, uint64_t* lo = nullptr, uint64_t* hi = nullptr);
bool write_watch_fault(void* at);
// NX_LATE_GUARD=1 (write_watch.cpp): the whole pages of the ranges a released guest should not touch before their
// write-backs land are made inaccessible, and each first access is recorded and let through.
struct LateGuardEvent {
    uint64_t addr, pc, tid;
    bool write;
};
bool late_guard_on();
uint64_t late_guard_add(uint64_t addr, uint64_t size);  // the bytes it covers
uint32_t late_guard_open(LateGuardEvent* out, uint32_t room);  // everything accessible again; the accesses since the adds
bool late_guard_fault(void* at, bool write, uint64_t pc);
// NX_SUBMIT_DIAG: sampled active watch intervals, not a history of protected pages.
struct WriteWatchSnapshot {
    struct Range { uint64_t begin = 0, end = 0; } ranges[16];
    uint64_t count = 0, adds = 0, takes = 0, removes = 0, faults = 0;
};
WriteWatchSnapshot write_watch_snapshot();
// NX_HEADLESS=1: no visible window (it is created hidden) and nothing presented; frames still render and
// NX_DUMP_FRAMES still writes them. For unattended test runs.
bool headless();
// Live settings (settings_live.cpp): read once from the player's settings, then changed by the settings panel.
int model_lod();  // 1: model_lod = highest (b3_model_lod.cpp), 0: the game's own
void set_model_lod(int v);
void start_code_preload();
void lower_thread_priority();  // the calling thread at the host's lowest (background work; code_preload.cpp)  // read the program's code into memory in the background (code_preload.cpp)
// Ends the process when its memory footprint passes NX_MEM_LIMIT_GB (default 8); logs it every 30 s (macOS).
void start_memory_guard();
// How much of [from, to) (monotonic ns) the memory guard spent reading (memguard_android.cpp; 0 elsewhere).
uint64_t memguard_overlap_ns(uint64_t from, uint64_t to);
// The calling thread's CPU time, page faults (major, minor) and context switches (voluntary, involuntary)
// (frame_stats.cpp; Linux and Android, zeros elsewhere).
void thread_counters(uint64_t& cpu_ns, long faults[2], long switches[2]);
void input_script(PadState& p);  // NX_INPUT_SCRIPT presses (input_script.cpp), for unattended runs

// Audio: s16 stereo 48 kHz. `done` may run on any thread once the backend releases the submitted bytes.
// SDL can still have those samples queued; buffer release alone is not a playback-position clock.
using AudioDone = void (*)(void* ctx);
bool audio_open(AudioDone done);
bool audio_submit(const void* data, size_t bytes, void* ctx);  // false: no device (caller paces itself)
uint32_t audio_queued();
void audio_pause(bool paused);  // app in the background

// Vulkan: the loader's vkGetInstanceProcAddr and the surface extension of this platform.
void* vulkan_get_instance_proc_addr();
const char* vulkan_surface_extension();
// Creates the platform surface for `native_window` (VkInstance, VkSurfaceKHR*), returns a VkResult.
int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa);

}  // namespace nx::plat
