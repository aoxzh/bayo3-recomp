// Windows host: memory, threads, time, files, the game window (F11 / Alt+Enter fullscreen), XInput + keyboard,
// XAudio2, the Vulkan loader and win32 surface, crash handler, hardware watchpoints (NX_WATCH) and main().

// SetProcessDpiAwarenessContext is Windows 10 1703 (0x0A00) and newer. MinGW's <_mingw.h> defaults _WIN32_WINNT to
// 0x601 when it is unset, and windef.h derives WINVER from it (hiding the DPI calls behind `#if WINVER >= 0x0605`).
// libc++'s <cstdlib> pulls <_mingw.h> in through <stdlib.h>, so the pin has to come before EVERY other include --
// an `#ifndef _WIN32_WINNT` guard further down would already be too late.
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#ifdef WINVER
#undef WINVER
#endif
#define WINVER _WIN32_WINNT

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <mmsystem.h>
#include <xaudio2.h>
#include <xinput.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>

namespace nx::nvn {
void pipeline_cache_flush();  // nvn_pipeline_cache.cpp; the platform's normal exit path calls it
}
#include <nx/runtime.h>

#include "kernel.h"
#include "pad_config.h"
#include "pad_rumble.h"
#include "platform.h"

namespace nx {
HWND g_window = nullptr;
void boot();
}  // namespace nx

namespace nx::plat {

// ---- memory ---------------------------------------------------------------------------------------------------

bool reserve(uint64_t addr, uint64_t size) {
    return VirtualAlloc(reinterpret_cast<void*>(addr), size, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
}

bool commit(uint64_t addr, uint64_t size) {
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    return VirtualAlloc(reinterpret_cast<void*>(a), e - a, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void decommit(uint64_t addr, uint64_t size) { VirtualFree(reinterpret_cast<void*>(addr), size, MEM_DECOMMIT); }

// ---- threads and time -----------------------------------------------------------------------------------------

struct ThreadStart {
    ThreadFn fn;
    void* arg;
};

static DWORD WINAPI thread_entry(void* p) {
    ThreadStart s = *static_cast<ThreadStart*>(p);
    delete static_cast<ThreadStart*>(p);
    s.fn(s.arg);
    return 0;
}

void start_thread(ThreadFn fn, void* arg, size_t stack_size) {
    CreateThread(nullptr, stack_size, thread_entry, new ThreadStart{fn, arg}, STACK_SIZE_PARAM_IS_A_RESERVATION,
                 nullptr);
}

void exit_thread() { ExitThread(0); }
void exit_process(int code) {
    nx::nvn::pipeline_cache_flush();  // the cache writer runs on its own thread; do not drop this scene's pipelines
    nx::log_flush();  // the logger writes on its own thread; ExitProcess would otherwise drop the last lines
    ExitProcess(UINT(code));
}

// Sleep precision. Windows' default scheduler tick is 15.6 ms, so sleep_for rounds any request below it up to that --
// the vsync thread then cannot produce a 120 Hz (8.3 ms) or 240 Hz (4.2 ms) clock at all, and the present pacing
// drifts by up to a tick. MEASURED: with the default tick the game held 62 fps median on a 240 Hz model and 68 on
// 120, while the GPU was idle; the same scenes reach 80-100 fps once the tick is 1 ms. timeBeginPeriod(1) is
// per-process on Windows 10 2004+, and it is paired with timeEndPeriod in exit_process and the atexit handler below.
static bool g_timer_period = false;
static void raise_timer_resolution() {
    if (g_timer_period) return;
    g_timer_period = timeBeginPeriod(1) == TIMERR_NOERROR;
}
static void drop_timer_resolution() {
    if (!g_timer_period) return;
    g_timer_period = false;
    timeEndPeriod(1);
}

void sleep_ns(int64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
void yield() { SwitchToThread(); }
void set_thread_name(const char*) {}
float default_render_scale() { return 1.0f; }
void release_window(void*) {}
uint64_t window_generation() { return 1; }
void audio_pause(bool) {}

std::string setting(const std::string& key) {
    std::string env = "NX_";
    for (char ch : key) env += char(std::toupper(static_cast<unsigned char>(ch)));
    const char* v = std::getenv(env.c_str());
    return v ? v : "";
}
int64_t current_thread_id() { return int64_t(GetCurrentThreadId()); }
bool profile(const int64_t*, const uint64_t* const*, size_t, int, const std::string&) { return false; }

uint64_t monotonic_ns() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(double(t.QuadPart) * 1e9 / double(f.QuadPart));
}

// ---- files and output -----------------------------------------------------------------------------------------

FILE* fopen_utf8(const std::string& path, const char* mode) {
    std::wstring wp = std::filesystem::u8path(path).wstring();
    std::wstring wm(mode, mode + std::strlen(mode));
    return _wfopen(wp.c_str(), wm.c_str());
}
int64_t ftell64(FILE* f) { return _ftelli64(f); }
int fseek64(FILE* f, int64_t off, int origin) { return _fseeki64(f, off, origin); }
uint64_t read_at(FILE* f, int64_t off, void* buf, uint64_t size) {
    _lock_file(f);  // seek + read as one step for this FILE
    _fseeki64_nolock(f, off, SEEK_SET);
    uint64_t n = _fread_nolock(buf, 1, size, f);
    _unlock_file(f);
    return n;
}
bool make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(path), ec);
    return !ec;
}

void log_line(const char* line) { std::fprintf(stderr, "%s\n", line); }

std::string data_root() {
    const char* r = std::getenv("NX_DATA_ROOT");
    return r ? r : nx::game::kInfo.pc_data_root;
}

// ---- window ---------------------------------------------------------------------------------------------------

static std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }  // ASCII titles
// The game renders its swapchain at the render resolution; the window can be any size (the compositor scales).

static constexpr DWORD kWindowed = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_placement{sizeof(WINDOWPLACEMENT)};

static void toggle_fullscreen(HWND h) {
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        GetWindowPlacement(h, &g_placement);
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, kWindowed);
        SetWindowPlacement(h, &g_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
}

// The mouse camera (NX_MOUSE_CAMERA, on unless 0): a click in the window captures the mouse (hidden, held in the
// window), its raw motion is summed here and drained by read_pad into the right stick; losing the focus or F10 lets
// it go. Raw input (WM_INPUT) rather than cursor positions: no acceleration, and no edge where the cursor stops.
static std::atomic<bool> g_mouse_captured{false};
static std::atomic<int32_t> g_mouse_dx{0}, g_mouse_dy{0};
// The mouse settings, read from NX_MOUSE_* at start and changed live by the settings panel (F1).
static std::atomic<bool> g_mouse_on{true}, g_mouse_invert{false};
static std::atomic<float> g_mouse_sens{1.0f};
static void mouse_settings_from_env() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char* e = std::getenv("NX_MOUSE_CAMERA");
        g_mouse_on = !(e && e[0] == '0');
        e = std::getenv("NX_MOUSE_SENS");
        const float v = e ? float(std::atof(e)) : 1.0f;
        g_mouse_sens = v > 0 ? v : 1.0f;
        e = std::getenv("NX_MOUSE_INVERT_Y");
        g_mouse_invert = e && e[0] == '1';
    });
}

static bool mouse_camera_on() {
    mouse_settings_from_env();
    return g_mouse_on && !headless();
}

static void mouse_capture(HWND h, bool on) {
    if (on == g_mouse_captured.load()) return;
    g_mouse_captured = on;
    if (on) {
        RECT r;
        GetClientRect(h, &r);
        MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
        while (ShowCursor(FALSE) >= 0) {}
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
    g_mouse_dx = 0, g_mouse_dy = 0;
}

// ---- the settings panel (F1, or + and - together on a controller) --------------------------------------------------
// Witch Time's F1 panel, for this runtime's settings: drawn here with GDI into an RGBA image that the present step lays
// over the picture (settings_panel(), nvn_vk.cpp), driven by the arrow keys or the d-pad while the game gets no input.
// A change is written to the settings file the runner names (NX_SETTINGS_FILE: private/b3.conf, the launcher's), keeping
// its comments. Window mode and the mouse apply at once; the rest at the next start, and the panel says so.
static constexpr UINT kToggleFullscreen = WM_APP + 1;  // wparam: 1 fullscreen, 0 windowed (the window thread's job)
static constexpr UINT kReleaseMouse = WM_APP + 2;      // ShowCursor counts per thread: the window thread's too
static std::atomic<bool> g_panel_open{false};

namespace {

struct PanelItem {
    const char* label;
    const char* key;  // the b3.conf key (NX_<KEY>)
    std::vector<std::pair<const char*, const char*>> values;  // (value, shown); "" = the key absent
    bool live;
    int index = 0, start = 0;
};

std::mutex g_panel_lock;
std::vector<PanelItem> g_items;
int g_selected = 0;
std::string g_panel_note;
std::vector<uint8_t> g_panel_rgba;
uint32_t g_panel_w = 0, g_panel_h = 0, g_panel_target = 0;
uint64_t g_panel_version = 0;
bool g_panel_dirty = true;

void panel_init() {  // under g_panel_lock
    if (!g_items.empty()) return;
    g_items = {
        {"Window", "fullscreen", {{"0", "Windowed"}, {"1", "Fullscreen"}}, true},
        {"Window size", "resolution",
         {{"1280x720", "1280 x 720"}, {"1600x900", "1600 x 900"}, {"1920x1080", "1920 x 1080"},
          {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160"}, {"auto", "Fit the display"}}, false},
        {"Frame rate", "display_hz",
         {{"", "Display rate"}, {"60", "60"}, {"90", "90"}, {"120", "120"}, {"144", "144"}}, false},
        {"V-sync", "fifo", {{"0", "Off"}, {"1", "On"}}, false},
        {"Anisotropic filtering", "aniso",
         {{"", "Game default"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}}, false},
        {"FSR 1 upscale", "fsr", {{"0", "Off"}, {"1", "On"}}, false},
        {"Model detail", "model_lod", {{"", "Game default"}, {"highest", "Highest"}}, true},
        {"Film grain", "film_grain", {{"", "On"}, {"off", "Off"}}, true},
        {"Controller layout", "pad_swap", {{"0", "Nintendo"}, {"1", "Xbox"}}, false},
        {"Mouse camera", "mouse_camera", {{"1", "On"}, {"0", "Off"}}, true},
        {"Mouse speed", "mouse_sens",
         {{"0.5", "0.5"}, {"0.75", "0.75"}, {"", "1.0"}, {"1.5", "1.5"}, {"2", "2.0"}, {"3", "3.0"}}, true},
        {"Invert mouse Y", "mouse_invert_y", {{"0", "Off"}, {"1", "On"}}, true},
        {"Sound", "mute", {{"0", "On"}, {"1", "Muted"}}, false},
    };
    for (PanelItem& it : g_items) {
        std::string env = "NX_";
        for (const char* c = it.key; *c; c++) env += char(std::toupper(static_cast<unsigned char>(*c)));
        const char* now = std::getenv(env.c_str());
        // Absent means the first "" choice, else the first one (the runtime's default for a switch).
        int found = -1, blank = -1;
        for (int k = 0; k < int(it.values.size()); k++) {
            if (now && std::strcmp(now, it.values[k].first) == 0) found = k;
            if (!it.values[k].first[0] && blank < 0) blank = k;
        }
        it.index = it.start = found >= 0 ? found : blank >= 0 ? blank : 0;
    }
    if (const char* f = std::getenv("NX_FULLSCREEN"); f && f[0] == '1') g_items[0].index = g_items[0].start = 1;
}

// key=value into the settings file: the first line of the key is rewritten (its comment kept in its column), later
// duplicates dropped, a missing key appended. Written beside and renamed over, so a crash leaves the old file.
bool save_setting(const char* key, const char* value) {
    const char* path = std::getenv("NX_SETTINGS_FILE");
    if (!path || !*path) return false;
    std::vector<std::string> lines;
    if (FILE* f = fopen_utf8(path, "rb")) {
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        std::fclose(f);
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            at = end + 1;
        }
    }
    bool written = false;
    std::vector<std::string> out;
    for (const std::string& line : lines) {
        const size_t eq = line.find('='), hash = line.find('#');
        std::string k = eq == std::string::npos || (hash != std::string::npos && hash < eq) ? "" : line.substr(0, eq);
        while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.pop_back();
        while (!k.empty() && std::isspace(static_cast<unsigned char>(k.front()))) k.erase(0, 1);
        if (k != key) {
            out.push_back(line);
            continue;
        }
        if (written) continue;
        std::string fresh = std::string(key) + "=" + value;
        if (hash != std::string::npos) fresh += std::string(std::max<size_t>(1, hash > fresh.size() ? hash - fresh.size() : 1), ' ') + line.substr(hash);
        out.push_back(fresh);
        written = true;
    }
    if (!written) out.push_back(std::string(key) + "=" + value);
    const std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen_utf8(tmp, "wb");
    if (!f) return false;
    for (const std::string& line : out) std::fputs((line + "\n").c_str(), f);
    std::fclose(f);
    auto utf16 = [](const std::string& s) {
        std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
        return w;
    };
    return MoveFileExW(utf16(tmp).c_str(), utf16(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void panel_apply(PanelItem& it) {  // under g_panel_lock
    const char* value = it.values[it.index].first;
    if (std::strcmp(it.key, "fullscreen") == 0 && g_window) PostMessageW(g_window, kToggleFullscreen, it.index, 0);
    if (std::strcmp(it.key, "mouse_camera") == 0) g_mouse_on = it.index == 0;
    if (std::strcmp(it.key, "mouse_sens") == 0) g_mouse_sens = value[0] ? float(std::atof(value)) : 1.0f;
    if (std::strcmp(it.key, "mouse_invert_y") == 0) g_mouse_invert = it.index == 1;
    if (std::strcmp(it.key, "model_lod") == 0) set_model_lod(it.index == 1);
    if (std::strcmp(it.key, "film_grain") == 0) set_film_grain_off(it.index == 1);
    if (save_setting(it.key, value))
        g_panel_note = "Saved.";
    else
        g_panel_note = "Not saved: no settings file (start the game with tools/b3_win.py play).";
    log("settings panel: %s = %s", it.key, value[0] ? value : "(default)");
}

enum PanelKey { kPanelUp, kPanelDown, kPanelLeft, kPanelRight, kPanelBack };

void panel_key(PanelKey key) {
    std::lock_guard lk(g_panel_lock);
    panel_init();
    PanelItem& it = g_items[g_selected];
    const int n = int(it.values.size());
    switch (key) {
        case kPanelUp: g_selected = (g_selected + int(g_items.size()) - 1) % int(g_items.size()); break;
        case kPanelDown: g_selected = (g_selected + 1) % int(g_items.size()); break;
        case kPanelLeft: it.index = (it.index + n - 1) % n, panel_apply(it); break;
        case kPanelRight: it.index = (it.index + 1) % n, panel_apply(it); break;
        case kPanelBack: g_panel_open = false; break;
    }
    g_panel_dirty = true;
}

void panel_toggle() {
    const bool open = !g_panel_open;
    if (open) {
        std::lock_guard lk(g_panel_lock);
        panel_init();
        g_panel_note.clear();
        g_panel_dirty = true;
    }
    g_panel_open = open;
    if (open && g_window) PostMessageW(g_window, kReleaseMouse, 0, 0);
    log("settings panel %s", open ? "opened" : "closed");
}

std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// The panel's pixels at a size that suits the swapchain's height (crisp text, not a scaled bitmap).
void panel_render(uint32_t target_h) {  // under g_panel_lock
    const int ph = std::clamp(int(target_h * 0.72f), 360, 1500), pw = ph * 4 / 3;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = pw, bi.bmiHeader.biHeight = -ph;  // top-down
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    auto fill = [&](RECT r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c);
        FillRect(dc, &r, b);
        DeleteObject(b);
    };
    fill({0, 0, pw, ph}, RGB(122, 84, 160));                  // the frame
    fill({3, 3, pw - 3, ph - 3}, RGB(20, 17, 30));             // the panel
    const int unit = ph / 23, pad = unit;  // title 3.5 units, 11 rows of 1.5, the note: 22.5
    auto font = [&](int height, int weight) {
        return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT title = font(unit * 3 / 2, FW_SEMIBOLD), body = font(unit, FW_NORMAL), small = font(unit * 3 / 4, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](HFONT f, COLORREF c, RECT r, const std::string& s, UINT align) {
        SelectObject(dc, f);
        SetTextColor(dc, c);
        std::wstring w = wide(s);
        DrawTextW(dc, w.c_str(), int(w.size()), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };
    text(title, RGB(240, 232, 255), {pad, pad / 2, pw - pad, pad / 2 + unit * 2}, "Settings", DT_LEFT);
    text(small, RGB(170, 160, 190), {pad, pad / 2, pw - pad, pad / 2 + unit * 2},
         "Up/Down choose   Left/Right change   Esc / F1 close", DT_RIGHT);
    const int top = pad / 2 + unit * 3, row = unit * 3 / 2;
    for (int k = 0; k < int(g_items.size()); k++) {
        const PanelItem& it = g_items[k];
        RECT r{pad, top + k * row, pw - pad, top + (k + 1) * row};
        const bool sel = k == g_selected;
        if (sel) fill(r, RGB(64, 44, 92));
        RECT label = r, value = r;
        label.left += unit / 2, value.right -= unit / 2;
        text(body, sel ? RGB(255, 255, 255) : RGB(205, 198, 220), label, it.label, DT_LEFT);
        std::string v = it.values[it.index].second;
        if (sel) v = "<  " + v + "  >";
        if (!it.live && it.index != it.start) v += "   (next start)";
        text(body, sel ? RGB(255, 214, 120) : RGB(225, 215, 240), value, v, DT_RIGHT);
    }
    const int note_y = top + int(g_items.size()) * row + unit / 2;
    text(small, RGB(170, 160, 190), {pad, note_y, pw - pad, note_y + unit * 3 / 2},
         g_panel_note.empty() ? "Changes are saved to the settings file the launcher uses." : g_panel_note, DT_LEFT);
    GdiFlush();
    g_panel_rgba.resize(size_t(pw) * ph * 4);
    const uint8_t* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < size_t(pw) * ph; i++) {  // BGRX -> RGBA
        g_panel_rgba[4 * i] = src[4 * i + 2], g_panel_rgba[4 * i + 1] = src[4 * i + 1];
        g_panel_rgba[4 * i + 2] = src[4 * i], g_panel_rgba[4 * i + 3] = 255;
    }
    for (HFONT f : {title, body, small}) DeleteObject(f);
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    g_panel_w = uint32_t(pw), g_panel_h = uint32_t(ph), g_panel_target = target_h;
    g_panel_version++;
    g_panel_dirty = false;
}

}  // namespace

static void panel_start_open() {  // NX_SETTINGS_PANEL=1: open from the start (screenshots, tests)
    static std::once_flag once;
    std::call_once(once, [] {
        const char* e = std::getenv("NX_SETTINGS_PANEL");
        if (e && e[0] == '1') g_panel_open = true;
    });
}

bool settings_panel_open() {
    panel_start_open();
    return g_panel_open;
}

const uint8_t* settings_panel(uint32_t target_h, uint32_t& w, uint32_t& h, uint64_t& version) {
    panel_start_open();
    if (!g_panel_open) return nullptr;
    std::lock_guard lk(g_panel_lock);
    panel_init();
    if (g_panel_dirty || g_panel_target != target_h) {
        panel_render(target_h);
        // NX_SETTINGS_PANEL_DUMP=1: each new picture to <data root>/settings_panel.ppm (checking the drawing).
        if (const char* e = std::getenv("NX_SETTINGS_PANEL_DUMP"); e && e[0] == '1' && !g_panel_rgba.empty())
            if (FILE* f = fopen_utf8(data_root() + "/settings_panel.ppm", "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", g_panel_w, g_panel_h);
                for (size_t i = 0; i < size_t(g_panel_w) * g_panel_h; i++) std::fwrite(&g_panel_rgba[4 * i], 1, 3, f);
                std::fclose(f);
            }
    }
    if (g_panel_rgba.empty()) return nullptr;
    w = g_panel_w, h = g_panel_h, version = g_panel_version;
    return g_panel_rgba.data();
}

// ---- Discord Rich Presence (NX_DISCORD_APP_ID) ---------------------------------------------------------------------
// The running Discord client's local IPC: \\.\pipe\discord-ipc-<0..9>, frames of {uint32 opcode, uint32 length, JSON}.
// A handshake (opcode 0) with the application id, then SET_ACTIVITY (opcode 1): what is being played and since when.
// The id is a Discord application the player registers (its name is what Discord shows); none is built in, so the
// presence is off until b3.conf has discord_app_id=<id>. Discord not running: tried again every 15 s, silently.
namespace {

bool discord_send(HANDLE pipe, uint32_t op, const std::string& json) {
    std::string frame(8, '\0');
    const uint32_t len = uint32_t(json.size());
    std::memcpy(&frame[0], &op, 4), std::memcpy(&frame[4], &len, 4);
    frame += json;
    DWORD wrote = 0;
    return WriteFile(pipe, frame.data(), DWORD(frame.size()), &wrote, nullptr) && wrote == frame.size();
}

bool discord_read(HANDLE pipe, std::string& json) {
    uint32_t head[2];
    DWORD got = 0;
    if (!ReadFile(pipe, head, 8, &got, nullptr) || got != 8 || head[1] > (1u << 20)) return false;
    json.assign(head[1], '\0');
    return head[1] == 0 || (ReadFile(pipe, json.data(), head[1], &got, nullptr) && got == head[1]);
}

void discord_thread(std::string app_id) {
    // Program 1 is the Bayonetta Origins prologue the hub's picture book starts; the presence says which one runs.
    const bool book = nx::game::kInfo.program_id == 0x01004a4010fea001ull;
    const long long start = (long long)std::time(nullptr);
    bool said = false, missing_said = false;
    log("discord: presence on (application %s)", app_id.c_str());
    for (;;) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        DWORD error = 0;
        for (int k = 0; k < 10 && pipe == INVALID_HANDLE_VALUE; k++) {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(k);
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE && k == 0) error = GetLastError();
        }
        if (pipe == INVALID_HANDLE_VALUE && !missing_said)
            log("discord: no Discord client running (%lu); trying every 15 s", error), missing_said = true;
        if (pipe != INVALID_HANDLE_VALUE) {
            std::string reply;
            const std::string activity =
                std::string("{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"1\",\"args\":{\"pid\":") +
                std::to_string(GetCurrentProcessId()) + ",\"activity\":{\"details\":\"" +
                (book ? "The Old Picture Book" : "Bayonetta 3") + "\",\"state\":\"" +
                (book ? "Bayonetta Origins prologue" : "Playing") + "\",\"timestamps\":{\"start\":" +
                std::to_string(start) + "}}}}";
            if (discord_send(pipe, 0, "{\"v\":1,\"client_id\":\"" + app_id + "\"}") && discord_read(pipe, reply) &&
                discord_send(pipe, 1, activity) && discord_read(pipe, reply)) {
                if (!said) log("discord: presence set (%s)", reply.find("\"ERROR\"") == std::string::npos ? "ok" : reply.c_str());
                said = true;
                for (;;) {  // keep the pipe open: Discord clears the activity when it closes
                    Sleep(15000);
                    if (!discord_send(pipe, 1, activity) || !discord_read(pipe, reply)) break;
                }
            }
            CloseHandle(pipe);
        }
        Sleep(15000);
    }
}

void discord_start() {
    const char* id = std::getenv("NX_DISCORD_APP_ID");
    if (!id || !*id || headless()) return;
    for (const char* c = id; *c; c++)
        if (!std::isdigit(static_cast<unsigned char>(*c))) {
            log("discord: NX_DISCORD_APP_ID must be the application's numeric id");
            return;
        }
    std::thread(discord_thread, std::string(id)).detach();
}

}  // namespace

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CLOSE:
            log("window closed");
            nx::nvn::pipeline_cache_flush();
            log_flush();
            ExitProcess(0);
        case WM_INPUT:
            if (g_mouse_captured) {
                RAWINPUT ri;
                UINT size = sizeof ri;
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) !=
                        UINT(-1) && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    g_mouse_dx += ri.data.mouse.lLastX;
                    g_mouse_dy += ri.data.mouse.lLastY;
                }
            }
            break;
        case kToggleFullscreen:
            if ((w != 0) != g_fullscreen) toggle_fullscreen(h);
            return 0;
        case kReleaseMouse:
            mouse_capture(h, false);
            return 0;
        case WM_LBUTTONDOWN:
            if (mouse_camera_on() && !g_mouse_captured && !g_panel_open) {
                mouse_capture(h, true);
                return 0;  // the capturing click is not a game input
            }
            break;
        case WM_KILLFOCUS:
            mouse_capture(h, false);
            break;
        case WM_SIZE:
        case WM_MOVE:
            if (g_mouse_captured) mouse_capture(h, false), mouse_capture(h, true);  // re-clip to the new rect
            break;
        case WM_KEYDOWN:
            if (w == VK_F11) toggle_fullscreen(h);
            if (w == VK_F10) mouse_capture(h, false);
            if (w == VK_F1 && !(l & (1 << 30))) {  // not the auto-repeat of a held key
                panel_toggle();
                return 0;
            }
            if (g_panel_open) {  // the panel's keys; the game reads none while it is open (read_pad)
                switch (w) {
                    case VK_UP: case 'W': panel_key(kPanelUp); break;
                    case VK_DOWN: case 'S': panel_key(kPanelDown); break;
                    case VK_LEFT: case 'A': panel_key(kPanelLeft); break;
                    case VK_RIGHT: case 'D': case VK_RETURN: case VK_SPACE: panel_key(kPanelRight); break;
                    case VK_ESCAPE: case VK_BACK: panel_key(kPanelBack); break;
                }
                return 0;
            }
            break;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN) {  // Alt+Enter
                toggle_fullscreen(h);
                return 0;
            }
            break;
        case WM_SYSKEYUP:
            if (w == VK_F10) {  // F10 is a system key: it arrives here, and DefWindowProc would open the menu loop
                mouse_capture(h, false);
                return 0;
            }
            break;
        case WM_SYSCHAR:
            if (w == VK_RETURN) return 0;  // no beep
            break;
    }
    return DefWindowProcW(h, msg, w, l);
}

// The window lives on its own thread so its message loop keeps running whatever the game threads do.
static void window_thread(std::atomic<bool>* ready) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    const std::wstring cls = widen(nx::game::kInfo.title) + L"Recomp";  // tools/capture.ps1 finds the window by it
    wc.lpszClassName = cls.c_str();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    // The render resolution's aspect (NX_RESOLUTION, default 16:9), about 80% of the screen height, centred.
    unsigned rw = 16, rh = 9;
    if (const char* r = std::getenv("NX_RESOLUTION")) std::sscanf(r, "%ux%u", &rw, &rh);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int ch = std::max(540, sh * 8 / 10), cw = int(int64_t(ch) * rw / std::max(1u, rh));
    RECT r{0, 0, cw, ch};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (sh - h) / 2;
    // NX_HEADLESS: created hidden (the surface still needs a window), never brought to the front.
    const DWORD style = headless() ? (kWindowed & ~WS_VISIBLE) : kWindowed;
    g_window = CreateWindowW(wc.lpszClassName, (widen(nx::game::kInfo.title) + L" (Switch recomp)  -  F11: fullscreen").c_str(), style,
                             std::max(0, x), std::max(0, y), w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (const char* f = std::getenv("NX_FULLSCREEN"); f && *f == '1' && !headless()) toggle_fullscreen(g_window);
    discord_start();
    if (!headless()) {  // always: the settings panel can turn the mouse camera on later
        RAWINPUTDEVICE rid{0x01, 0x02, 0, g_window};  // generic desktop, mouse
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) log("input: no raw mouse input (%lu)", GetLastError());
    }
    if (!headless()) SetForegroundWindow(g_window);
    *ready = true;
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void* create_window() {
    if (!g_window) {
        static std::atomic<bool> ready{false};
        std::thread(window_thread, &ready).detach();
        while (!ready) Sleep(1);
    }
    return g_window;
}

bool window_minimized() { return g_window && IsIconic(g_window); }

void window_size(uint32_t& w, uint32_t& h) {
    RECT r{};
    if (g_window) GetClientRect(g_window, &r);
    w = uint32_t(r.right - r.left);
    h = uint32_t(r.bottom - r.top);
}

void screen_size(uint32_t& w, uint32_t& h) { window_size(w, h); }

// The panel's refresh rate. The window's current mode is what the swapchain is actually paced against, so ask the
// monitor the window is on; a 119 Hz panel is what the runtime should model, not 60 (measured: at a 60 Hz model the
// game's own interval rule pinned it to 60 while the GPU had 13 ms of headroom). Cached: the mode only changes on a
// resolution/refresh switch, and this is called from the vsync thread's period computation.
uint32_t screen_refresh_hz() {
    static const uint32_t hz = [] () -> uint32_t {
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        const HMONITOR mon = g_window ? MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY) : nullptr;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!mon || !GetMonitorInfoW(mon, &mi)) return 0;
        if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
        const uint32_t rate = dm.dmDisplayFrequency;
        // 0 and 1 are the "hardware default" sentinels, and anything outside this range is not a panel.
        return rate >= 24 && rate <= 480 ? rate : 0;
    }();
    return hz;
}

// ---- input: XInput pad 0 + keyboard ---------------------------------------------------------------------------

static bool key(int vk) { return g_window && GetForegroundWindow() == g_window && (GetAsyncKeyState(vk) & 0x8000); }

// Keyboard and mouse bindings. Every pad input has a default set of keys; NX_KEY_<INPUT>=<key>[,<key>...] (b3.conf:
// key_<input>=...) replaces that set, and an empty value or `none` unbinds it. Inputs: A B X Y L R ZL ZR PLUS MINUS
// LSTICK RSTICK (the stick clicks), UP DOWN LEFT RIGHT (the d-pad), MOVE_UP MOVE_DOWN MOVE_LEFT MOVE_RIGHT (the left
// stick), CAM_UP CAM_DOWN CAM_LEFT CAM_RIGHT (the right stick). Keys are named as on the keyboard: A..Z, 0..9, F1..F9,
// Space, Enter, Escape, Tab, Backspace, Shift/LShift/RShift, Ctrl/LCtrl/RCtrl, Alt, Up, Down, Left, Right, Insert,
// Delete, Home, End, PageUp, PageDown, Numpad0..Numpad9, Mouse1 (left) .. Mouse5. F1 (the settings panel), F10 and
// F11 stay the window's own.
namespace {

struct KeyName {
    const char* name;
    int vk;
};
constexpr KeyName kKeyNames[] = {
    {"space", VK_SPACE}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE},
    {"tab", VK_TAB}, {"backspace", VK_BACK}, {"shift", VK_SHIFT}, {"lshift", VK_LSHIFT}, {"rshift", VK_RSHIFT},
    {"ctrl", VK_CONTROL}, {"lctrl", VK_LCONTROL}, {"rctrl", VK_RCONTROL}, {"alt", VK_MENU}, {"up", VK_UP},
    {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"insert", VK_INSERT}, {"delete", VK_DELETE},
    {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT}, {"mouse1", VK_LBUTTON},
    {"mouse2", VK_RBUTTON}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1}, {"mouse5", VK_XBUTTON2},
};

int key_code(std::string name) {
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
        return std::toupper(static_cast<unsigned char>(name[0]));
    if (name.size() == 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '9') return VK_F1 + (name[1] - '1');
    if (name.size() == 7 && name.compare(0, 6, "numpad") == 0 && std::isdigit(static_cast<unsigned char>(name[6])))
        return VK_NUMPAD0 + (name[6] - '0');
    for (const KeyName& k : kKeyNames)
        if (name == k.name) return k.vk;
    return 0;
}

enum Axis { kNone, kLx, kLy, kRx, kRy };
struct Binding {
    const char* input;
    uint64_t button;  // or 0 for a stick direction
    Axis axis;
    int sign;
    const char* keys;  // the default
    std::vector<int> vk;
};

// The defaults: WASD move, the arrows move and press the d-pad (some menus, the save browser, only read the d-pad),
// Space A, Shift B, J X, K Y, Q/E L/R, Z/C ZL/ZR, Enter +, Escape -, the left mouse button Y and the right X (the
// attacks), the middle one the right stick click; the numpad's 8456 turn the camera.
std::vector<Binding> make_bindings() {
    std::vector<Binding> b = {
        {"A", kA, kNone, 0, "Space"}, {"B", kB, kNone, 0, "Shift"}, {"X", kX, kNone, 0, "J,Mouse2"},
        {"Y", kY, kNone, 0, "K,Mouse1"}, {"L", kL, kNone, 0, "Q"}, {"R", kR, kNone, 0, "E"},
        {"ZL", kZL, kNone, 0, "Z"}, {"ZR", kZR, kNone, 0, "C"}, {"PLUS", kPlus, kNone, 0, "Enter"},
        {"MINUS", kMinus, kNone, 0, "Escape"}, {"LSTICK", kStickL, kNone, 0, "V"}, {"RSTICK", kStickR, kNone, 0, "Mouse3"},
        {"UP", kUp, kNone, 0, "Up"}, {"DOWN", kDown, kNone, 0, "Down"}, {"LEFT", kLeft, kNone, 0, "Left"},
        {"RIGHT", kRight, kNone, 0, "Right"}, {"MOVE_UP", 0, kLy, 1, "W,Up"}, {"MOVE_DOWN", 0, kLy, -1, "S,Down"},
        {"MOVE_LEFT", 0, kLx, -1, "A,Left"}, {"MOVE_RIGHT", 0, kLx, 1, "D,Right"},
        {"CAM_UP", 0, kRy, 1, "Numpad8"}, {"CAM_DOWN", 0, kRy, -1, "Numpad5"},
        {"CAM_LEFT", 0, kRx, -1, "Numpad4"}, {"CAM_RIGHT", 0, kRx, 1, "Numpad6"},
    };
    for (Binding& k : b) {
        const std::string env = std::string("NX_KEY_") + k.input;
        const char* given = std::getenv(env.c_str());
        std::string list = given ? given : k.keys;
        size_t at = 0;
        while (at <= list.size()) {
            size_t end = list.find_first_of(",+ ", at);
            if (end == std::string::npos) end = list.size();
            std::string name = list.substr(at, end - at);
            at = end + 1;
            if (name.empty() || name == "none") continue;
            if (int vk = key_code(name))
                k.vk.push_back(vk);
            else
                log("input: %s: unknown key '%s' (docs/PLAYING.md lists the names)", env.c_str(), name.c_str());
        }
        if (given) log("input: %s = %s", k.input, list.c_str());
    }
    return b;
}

// Raw mouse counts -> right stick: NX_MOUSE_SENS (default 1.0) scales, NX_MOUSE_INVERT_Y flips. About 20 counts in one
// poll is full deflection at 1.0; the motion is drained each poll, so the camera stops when the mouse does.
void mouse_stick(int32_t& rx, int32_t& ry) {
    mouse_settings_from_env();
    const float sens = g_mouse_sens;
    const bool invert = g_mouse_invert;
    const int32_t dx = g_mouse_dx.exchange(0), dy = g_mouse_dy.exchange(0);
    if (!g_mouse_captured || (!dx && !dy)) return;
    auto axis = [&](int32_t base, float counts) {
        const float v = float(base) + counts * sens * (float(kStickMax) / 20.0f);
        return int32_t(std::clamp(v, -float(kStickMax), float(kStickMax)));
    };
    rx = axis(rx, float(dx));
    ry = axis(ry, float(invert ? dy : -dy));  // screen y grows downwards, the stick's upwards
}

}  // namespace

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void read_pad(PadState& p) {
    // The game's polling and the settings panel's hold loop (nvn_queue.cpp) may both read; the edges below are shared.
    static std::mutex lock;
    std::lock_guard lk(lock);
    // The player's controller settings (pad_config.h), the same switches the Linux and macOS hosts read.
    static const nx::plat::PadConfig cfg = nx::plat::pad_config_from_env();
    static bool logged = false;
    // Rumble state, mirroring platform_linux.cpp:295-300 and :362-374. XInputSetState takes 0..65535 per motor and
    // keeps vibrating until told otherwise, so an effect is refreshed while active and explicitly stopped once it
    // ends -- a stopped process must not leave the controller buzzing.
    static uint64_t last_rumble = UINT64_MAX;
    static uint64_t rumble_at = 0;
    static bool rumble_error_logged = false;
    static bool pad_open = false;
    XINPUT_STATE xs{};
    const DWORD slot = DWORD(cfg.index < 4 ? cfg.index : 0);  // XInput addresses players 0..3
    if (XInputGetState(slot, &xs) == ERROR_SUCCESS) {
        const auto& g = xs.Gamepad;
        if (!pad_open) {
            // Do not replay commands sent while no physical pad was open, and drop the previous device's motors.
            reset_pad_rumble();
            last_rumble = UINT64_MAX;
            rumble_error_logged = false;
            pad_open = true;
            XINPUT_CAPABILITIES caps{};
            const bool caps_ok = XInputGetCapabilities(slot, 0, &caps) == ERROR_SUCCESS;
            log("input: player 1 is XInput controller %u, %s layout, deadzone %d, trigger %d, rumble %s", unsigned(slot),
                cfg.swap_face ? "Xbox" : "Nintendo", cfg.deadzone, cfg.trigger,
                caps_ok ? ((caps.Vibration.wLeftMotorSpeed || caps.Vibration.wRightMotorSpeed) ? "available"
                                                                                             : "unavailable")
                        : "unknown");
            logged = true;
        }
        if (g.wButtons & XINPUT_GAMEPAD_B) p.buttons |= kA;  // Nintendo layout
        if (g.wButtons & XINPUT_GAMEPAD_A) p.buttons |= kB;
        if (g.wButtons & XINPUT_GAMEPAD_Y) p.buttons |= kX;
        if (g.wButtons & XINPUT_GAMEPAD_X) p.buttons |= kY;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) p.buttons |= kL;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) p.buttons |= kR;
        // XInput triggers are 0..255; NX_PAD_TRIGGER is in the Npad's 0..32767 range.
        const int trigger = std::max(1, cfg.trigger * 255 / 32767);
        if (g.bLeftTrigger > trigger) p.buttons |= kZL;
        if (g.bRightTrigger > trigger) p.buttons |= kZR;
        if (g.wButtons & XINPUT_GAMEPAD_START) p.buttons |= kPlus;
        if (g.wButtons & XINPUT_GAMEPAD_BACK) p.buttons |= kMinus;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) p.buttons |= kStickL;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) p.buttons |= kStickR;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) p.buttons |= kLeft;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) p.buttons |= kUp;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) p.buttons |= kRight;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) p.buttons |= kDown;
        if (cfg.swap_face) p.buttons = nx::plat::pad_swap_face(p.buttons);
        auto dz = [&](int v) { return nx::plat::pad_axis(v, cfg.deadzone, kStickMax); };
        p.lx = dz(g.sThumbLX);
        p.ly = dz(g.sThumbLY);
        p.rx = dz(g.sThumbRX);
        p.ry = dz(g.sThumbRY);
        nx::plat::pad_camera(p.rx, p.ry, cfg);

        // Actuators (hle_hid.cpp publishes the guest's vibration into pad_rumble.h). XInput has two motors, so the
        // low and high half of each side are maxed the way platform_linux.cpp does with SDL_RumbleGamepad.
        const uint32_t rumble = read_pad_rumble();
        const uint64_t now = GetTickCount64();
        const bool changed = last_rumble != rumble;
        if ((changed && (rumble || last_rumble != UINT64_MAX)) || (rumble && now - rumble_at >= 250)) {
            XINPUT_VIBRATION vib{};
            vib.wLeftMotorSpeed = uint16_t(rumble);
            vib.wRightMotorSpeed = uint16_t(rumble >> 16);
            if (XInputSetState(slot, &vib) != ERROR_SUCCESS && !rumble_error_logged) {
                log("input: gamepad rumble unavailable (%lu)", GetLastError());
                rumble_error_logged = true;
            }
            last_rumble = rumble;
            rumble_at = now;
        }
    } else if (pad_open) {
        // Unplugged: stop the motors and invalidate the guest's cached actuator values (Linux does the same at
        // platform_linux.cpp:322-327). The next read_pad re-opens and re-logs the pad.
        XINPUT_VIBRATION vib{};
        XInputSetState(slot, &vib);
        reset_pad_rumble();
        last_rumble = UINT64_MAX;
        pad_open = false;
        logged = false;
        log("input: XInput controller %u disconnected", unsigned(slot));
    } else if (!logged && cfg.index > 0) {
        // Only worth saying when the player asked for a specific controller: index 0 is simply 'none connected'.
        log("input: waiting for controller %d (XInput slots are 0..3)", cfg.index);
        logged = true;
    }
    input_script(p);  // NX_INPUT_SCRIPT (input_script.cpp): unattended runs, which may drive the panel too
    // The settings panel from the controller: + and - together open or close it; while it is open the d-pad or the
    // left stick choose and change, A changes, B closes, and the game reads a neutral controller.
    static uint64_t panel_prev = 0;
    uint64_t panel_now = p.buttons;
    if (p.ly > kStickMax / 2) panel_now |= kUp;
    if (p.ly < -kStickMax / 2) panel_now |= kDown;
    if (p.lx < -kStickMax / 2) panel_now |= kLeft;
    if (p.lx > kStickMax / 2) panel_now |= kRight;
    const uint64_t pressed = panel_now & ~panel_prev;
    panel_prev = panel_now;
    if ((panel_now & (kPlus | kMinus)) == (kPlus | kMinus) && (pressed & (kPlus | kMinus))) {
        panel_toggle();
    } else if (g_panel_open) {
        if (pressed & kUp) panel_key(kPanelUp);
        if (pressed & kDown) panel_key(kPanelDown);
        if (pressed & kLeft) panel_key(kPanelLeft);
        if (pressed & (kRight | kA)) panel_key(kPanelRight);
        if (pressed & kB) panel_key(kPanelBack);
    }
    if (g_panel_open) {
        p = PadState{};
        return;
    }
    // Keyboard and mouse (the bindings above), with the controller at the same time: a held key or a moved mouse wins
    // its axis, buttons add up. The mouse buttons only count while the mouse is captured, so the click that captures
    // it and clicks on other windows are not attacks.
    static const std::vector<Binding> bindings = make_bindings();
    const bool focused = g_window && GetForegroundWindow() == g_window;
    for (const Binding& b : bindings) {
        bool down = false;
        for (int vk : b.vk) {
            const bool mouse = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
                               vk == VK_XBUTTON2;
            if (focused && (!mouse || g_mouse_captured) && (GetAsyncKeyState(vk) & 0x8000)) down = true;
        }
        if (!down) continue;
        if (b.button) {
            p.buttons |= b.button;
            continue;
        }
        int32_t& axis = b.axis == kLx ? p.lx : b.axis == kLy ? p.ly : b.axis == kRx ? p.rx : p.ry;
        axis = b.sign * kStickMax;
    }
    mouse_stick(p.rx, p.ry);
}

// ---- audio: XAudio2 -------------------------------------------------------------------------------------------

namespace {

std::atomic<uint32_t> g_queued{0};
AudioDone g_done = nullptr;

struct Voice : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
        auto* p = static_cast<std::pair<void*, std::vector<uint8_t>>*>(ctx);
        g_queued--;
        g_done(p->first);
        delete p;
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}
};

IXAudio2* g_xa = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
Voice g_callback;

}  // namespace

bool audio_open(AudioDone done) {
    g_done = done;
    // Headless test runs (and NX_MUTE=1) are silent: no device, the callers pace themselves with a timer.
    if (headless() || std::getenv("NX_MUTE")) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    IXAudio2MasteringVoice* master = nullptr;
    if (FAILED(g_xa->CreateMasteringVoice(&master))) return false;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 48000;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 48000 * 4;
    if (FAILED(g_xa->CreateSourceVoice(&g_voice, &wf, 0, 2.0f, &g_callback))) return false;
    g_voice->Start();
    return true;
}

bool audio_submit(const void* data, size_t bytes, void* ctx) {
    if (!g_voice) return false;
    auto* p = new std::pair<void*, std::vector<uint8_t>>(
        ctx, std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + bytes));
    XAUDIO2_BUFFER xb{};
    xb.AudioBytes = UINT32(bytes);
    xb.pAudioData = p->second.data();
    xb.pContext = p;
    g_queued++;
    g_voice->SubmitSourceBuffer(&xb);
    return true;
}

uint32_t audio_queued() { return g_queued; }

// ---- Vulkan ---------------------------------------------------------------------------------------------------

void* vulkan_get_instance_proc_addr() {
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr")) : nullptr;
}

const char* vulkan_surface_extension() { return VK_KHR_WIN32_SURFACE_EXTENSION_NAME; }

int vulkan_create_surface(void* instance, void* native_window, uint64_t* surface, void* gipa) {
    auto create = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(gipa)(static_cast<VkInstance>(instance), "vkCreateWin32SurfaceKHR"));
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = static_cast<HWND>(native_window);
    VkSurfaceKHR s = VK_NULL_HANDLE;
    VkResult r = create(static_cast<VkInstance>(instance), &ci, nullptr, &s);
    *surface = reinterpret_cast<uint64_t>(s);
    return r;
}

}  // namespace nx::plat

// ---- crash handler, watchpoints, entry --------------------------------------------------------------------------

namespace {

// NX_WATCH=<hex address>: hardware write watchpoint (DR0, 8 bytes) on the main thread; each write (up to 30) logs
// the host module/offset and the guest context.
uint64_t g_watch = 0;

void arm_watch(HANDLE thread) {
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(thread, &ctx);
    ctx.Dr0 = g_watch;
    ctx.Dr7 = (1ull << 0) | (1ull << 16) | (3ull << 18);  // L0, R/W0 = write, LEN0 = 8 bytes
    SetThreadContext(thread, &ctx);
}

void dump_guest(nx::CPU& c) {
    using namespace nx;
    log("  guest lr %s  sp %llx  tpidr %llx  tpidrro %llx", describe(c.x[30]).c_str(), (unsigned long long)c.x[31],
        (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    for (int k = 0; k < 32; k += 4)
        log("  x%-2d %016llx %016llx %016llx %016llx", k, (unsigned long long)c.x[k], (unsigned long long)c.x[k + 1],
            (unsigned long long)c.x[k + 2], (unsigned long long)c.x[k + 3]);
    backtrace(c);
}

uint64_t exe_va(void* p) { return uint64_t(p) - uint64_t(GetModuleHandleW(nullptr)) + 0x140000000ull; }

LONG WINAPI watch_handler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch) return EXCEPTION_CONTINUE_SEARCH;
    nx::log("WATCH: %llx written (now %llx) at %s.exe+%llx (tools/symbolize.py)", (unsigned long long)g_watch,
            (unsigned long long)nx::ld<uint64_t>(g_watch), nx::game::kInfo.id, (unsigned long long)exe_va(e->ExceptionRecord->ExceptionAddress));
    dump_guest(nx::current_cpu());
    static int hits = 0;
    if (++hits >= 30) e->ContextRecord->Dr7 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// "<module>+<offset>" for a host code address; our own exe as the linker's VA (tools/symbolize.py reads the map).
std::string host_where(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(pc), &m))
        return "?";
    char b[MAX_PATH + 32];
    if (m == GetModuleHandleW(nullptr)) {
        std::snprintf(b, sizeof b, "%s.exe+%llx", nx::game::kInfo.id, (unsigned long long)exe_va(reinterpret_cast<void*>(pc)));
        return b;
    }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::max(std::strrchr(path, '\\'), std::strrchr(path, '/'));
    std::snprintf(b, sizeof b, "%s+%llx", name ? name + 1 : path, (unsigned long long)(pc - uint64_t(m)));
    return b;
}

// The host call stack at the fault, unwound from its context with the x64 unwind tables.
void host_backtrace(const CONTEXT& at) {
    CONTEXT c = at;
    for (int k = 0; k < 16 && c.Rip; k++) {
        nx::log("  host #%-2d %s", k, host_where(c.Rip).c_str());
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!f) {  // a leaf function: the return address is at rsp
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &handler_data, &frame, nullptr);
    }
}

LONG WINAPI crash_handler(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    uint64_t info = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
    nx::log("CRASH: exception %08lx at %s (tools/symbolize.py for the exe), data address %llx", r->ExceptionCode,
            host_where(uint64_t(r->ExceptionAddress)).c_str(), (unsigned long long)info);
    host_backtrace(*e->ContextRecord);
    dump_guest(nx::current_cpu());
    nx::log_recent_files();
    nx::log_flush();  // the crash artifact must not lose the lines above
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

// NX_THROW_TRACE=1: log the host call stack of every C++ throw that is not a guest longjmp (first chance).
LONG WINAPI throw_tracer(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != 0xE06D7363) return EXCEPTION_CONTINUE_SEARCH;
    void* frames[24];
    USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::string s = "C++ throw, host stack:";
    for (USHORT k = 0; k < n; k++) {
        char b[32];
        std::snprintf(b, sizeof b, " %llx", (unsigned long long)exe_va(frames[k]));
        s += b;
    }
    nx::log("%s", s.c_str());
    return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
    // 1 ms scheduler tick: the vsync clock and the present pacing sleep for 4-17 ms and the default 15.6 ms tick
    // cannot express them (see raise_timer_resolution above).
    nx::plat::raise_timer_resolution();
    std::atexit(nx::plat::drop_timer_resolution);
    nx::plat::start_memory_guard();  // before anything can grow: a leak must never freeze the machine (memguard_win.cpp)
    SetUnhandledExceptionFilter(crash_handler);
    if (const char* t = std::getenv("NX_THROW_TRACE"); t && *t == '1') AddVectoredExceptionHandler(1, throw_tracer);
    if (const char* w = std::getenv("NX_WATCH")) {
        g_watch = std::strtoull(w, nullptr, 16);
        AddVectoredExceptionHandler(1, watch_handler);
        arm_watch(GetCurrentThread());
        nx::log("watching writes to %llx", (unsigned long long)g_watch);
    }
    try {
        nx::boot();
    } catch (const nx::LongJmp& j) {
        nx::fatal("longjmp to buffer %llx with no setjmp frame", (unsigned long long)j.buf);
    } catch (const std::exception& e) {
        nx::fatal("host exception: %s", e.what());
    }
    return 0;
}
