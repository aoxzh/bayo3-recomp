// nn::vi (display/layers) and nv:: (graphics driver setup) at the API level. The layer is the platform's window
// (Win32 window / Android surface); its handle is the "native window" the game hands to vkCreateViSurfaceNN, which
// the Vulkan bridge turns into the platform's surface.
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <nx/b3_scene_clock.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

// Recomputed SDK entry points passed through from the nv:: binds below: NvMMLite (the movie decoder)
// asserts on the session global at nvos_hos.cpp InitCheck ("Use of nv::InitializeGraphics is
// required.") when those calls are swallowed, so the guest nv layer must actually run.
void sdk_0041F020(nx::CPU&);  // nv::SetGraphicsAllocator
void sdk_0041F230(nx::CPU&);  // nv::InitializeGraphics

namespace nx {

// The host's display clock. NX_DISPLAY_HZ overrides it (30..360). Otherwise the panel's own refresh rate is used,
// because that is what the swapchain is paced against: on a 119 Hz panel a 60 Hz model left the game at 60 fps with
// 13 ms/frame of GPU headroom (docs/WINDOWS-PORT-2026-11-03.md §8). A host that cannot report its rate
// (screen_refresh_hz() == 0) keeps the historical 60 Hz.
//
// Raising it needs the clock conversions in b3_scene_clock.cpp / b3_fps_counters.cpp, which this rate turns on above
// 60: the game's gameplay step floors at one 60 Hz frame, so without them every frame at 120 Hz advanced 1/60 s and
// gameplay ran at 2.00x (measured 2026-11-09, `measured probe` game/wall; 1.00 with the conversion). Movies, audio
// and the fixed 30 fps scenes follow wall time either way (docs/WINDOWS-PORT-2026-11-03.md §9).
uint64_t display_period_ns() {
    static const uint64_t p = [] {
        const char* e = std::getenv("NX_DISPLAY_HZ");
        int hz = e ? std::atoi(e) : 0;
        const char* source = "NX_DISPLAY_HZ";
        if (hz < 30 || hz > 360) {
            hz = int(plat::screen_refresh_hz());
            source = "the panel";
        }
        if (hz < 30 || hz > 360) {
            b3::scene_clock_set_display_hz(60);
            return uint64_t(16'666'667);
        }
        b3::scene_clock_set_display_hz(hz);
        log("vi: display at %d Hz (%s)", hz, source);
        return uint64_t(1'000'000'000ull / uint64_t(hz));
    }();
    return p;
}

namespace {

constexpr uint64_t kFakeDisplay = kKernelBase - 0x1000;  // opaque pointers, never dereferenced by the game
constexpr uint64_t kFakeLayer = kKernelBase - 0x2000;

void ok(CPU& c) { c.x[0] = 0; }

void OpenDefaultDisplay(CPU& c) {
    st<uint64_t>(c.x[0], kFakeDisplay);
    c.x[0] = 0;
}

void CreateLayer(CPU& c) {
    void* w = plat::create_window();
    log("vi: layer created -> native window %p", w);
    plat::release_window(w);
    st<uint64_t>(c.x[0], kFakeLayer);
    c.x[0] = 0;
}

void GetNativeWindow(CPU& c) {  // (void** out, Layer*)
    st<uint64_t>(c.x[0], uint64_t(plat::create_window()));
    c.x[0] = 0;
}

void nv_noop(CPU&) {}

}  // namespace

// Recomputed SDK entry points the nv:: binds below pass through to; see the note at the top of the file.
namespace {

void nv_set_graphics_allocator(CPU& c) { sdk_0041F020(c); }
void nv_initialize_graphics(CPU& c) { sdk_0041F230(c); }

// The display's vsync: a kernel event a host thread signals every 1/60 s. The game gets it as an nn::os
// SystemEvent (AttachSystemEvent with our read/write handles, auto clear: each wait consumes one vsync).
std::shared_ptr<EventState> g_vsync = std::make_shared<EventState>();

void vsync_thread() {
    plat::set_thread_name("vsync");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += display_period_ns();
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        g_vsync->signaled = true;
        notify_all();
    }
}

void GetDisplayVsyncEvent(CPU& c) {  // (os::SystemEventType*, Display*)
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = g_vsync;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEventType; 1: AutoClear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    static std::once_flag once;
    std::call_once(once, [] { std::thread(vsync_thread).detach(); });
    c.x[0] = 0;
}

}  // namespace

NX_HLE("_ZN2nn2vi10InitializeEv", ok);
NX_HLE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", OpenDefaultDisplay);
NX_HLE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", CreateLayer);
NX_HLE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", GetNativeWindow);
NX_HLE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", GetDisplayVsyncEvent);
NX_HLE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_set_graphics_allocator);
NX_HLE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", nv_noop);
NX_HLE("_ZN2nv18InitializeGraphicsEPvm", nv_initialize_graphics);

}  // namespace nx
