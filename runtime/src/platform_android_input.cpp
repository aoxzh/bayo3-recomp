#include <cstdlib>
// Android host (NativeActivity): memory (mmap at fixed addresses), pthreads, the app's window (ANativeWindow),
// game controllers + touch, AAudio, the Vulkan loader and android surface, a signal crash handler and
// android_main. Game data lives in the app's external files dir (see docs/ANDROID.md):
//   <files>/analysis/{main,sdk}.bin   module images (tools/nso2bin.py)
//   <files>/extract/romfs/...         the RomFS
//   <files>/user/...                  saves
//   <files>/config.txt                optional: resolution=WxH
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <nx/game.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "pad_config.h"
#include "platform.h"
#include "platform_android.h"
#include "touch_art.h"
#include "touch_layout.h"
#include "touch_zones.h"

// Android input: game controllers and touch (split from platform_android.cpp).
namespace nx::plat {

// ---- input: game controllers + touch --------------------------------------------------------------------------
// Touch: left half = virtual stick (relative to where the finger went down); right half = four buttons in a
// diamond around the thumb (Y attack up-left, X special up-right, B dash down-left, A interact down-right), top
// corners L / R, top centre +.

namespace {
std::mutex g_in;
PadState g_pad_buttons;  // controller state
struct Touch {
    int32_t id;
    float x0, y0, x, y;
    bool stick;      // a floating stick: left (movement) or right (camera)
    bool camera;     // which of the two
    uint64_t button; // otherwise: the button it went down on (0: not used by the controls)
    bool dpad = false;  // on the D-pad: `button` is re-read from the finger's position at every move
};
std::vector<Touch> g_touches;
float g_w = 1, g_h = 1;
bool g_gamepad = false;  // the last input came from a controller: the drawn touch controls hide

uint64_t key_to_button(int32_t k) {
    switch (k) {
        case AKEYCODE_BUTTON_A: return kB;  // bottom face button = Nintendo B
        case AKEYCODE_BUTTON_B: return kA;  // right face button = Nintendo A
        case AKEYCODE_BUTTON_X: return kY;
        case AKEYCODE_BUTTON_Y: return kX;
        case AKEYCODE_BUTTON_L1: return kL;
        case AKEYCODE_BUTTON_R1: return kR;
        case AKEYCODE_BUTTON_L2: return kZL;
        case AKEYCODE_BUTTON_R2: return kZR;
        case AKEYCODE_BUTTON_START: return kPlus;
        case AKEYCODE_BUTTON_SELECT: return kMinus;
        case AKEYCODE_BUTTON_THUMBL: return kStickL;
        case AKEYCODE_BUTTON_THUMBR: return kStickR;
        case AKEYCODE_DPAD_LEFT: return kLeft;
        case AKEYCODE_DPAD_UP: return kUp;
        case AKEYCODE_DPAD_RIGHT: return kRight;
        case AKEYCODE_DPAD_DOWN: return kDown;
        case AKEYCODE_BACK: return kMinus;
        default: return 0;
    }
}

uint64_t touch_button(float x, float y) {
    float nx_ = x / g_w, ny = y / g_h;
    if (ny < 0.2f) return nx_ < 0.6f ? (nx_ < 0.45f ? kL : kPlus) : (nx_ > 0.85f ? kR : kZR);
    bool up = ny < 0.6f, right = nx_ > 0.8f;
    if (up) return right ? kX : kY;
    return right ? kA : kB;
}

}  // namespace

int32_t on_input(android_app*, AInputEvent* e) {
    std::lock_guard lk(g_in);
    int32_t type = AInputEvent_getType(e);
    int32_t src = AInputEvent_getSource(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        uint64_t b = key_to_button(AKeyEvent_getKeyCode(e));
        if (!b) return 0;
        g_gamepad = true;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN)
            g_pad_buttons.buttons |= b;
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP)
            g_pad_buttons.buttons &= ~b;
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        g_gamepad = true;
        // The launcher's controller settings (config.txt NX_PAD_*), as on the desktop backends.
        const PadConfig cfg = pad_config_from_env();
        auto axis = [&](int32_t a) {
            const float v = std::clamp(AMotionEvent_getAxisValue(e, a, 0), -1.f, 1.f);
            return pad_axis(int32_t(v * kStickMax), cfg.deadzone, kStickMax);
        };
        g_pad_buttons.lx = axis(AMOTION_EVENT_AXIS_X);
        g_pad_buttons.ly = -axis(AMOTION_EVENT_AXIS_Y);
        g_pad_buttons.rx = axis(AMOTION_EVENT_AXIS_Z);
        g_pad_buttons.ry = -axis(AMOTION_EVENT_AXIS_RZ);
        float hx = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_HAT_Y, 0);
        g_pad_buttons.buttons &= ~(kLeft | kRight | kUp | kDown | kZL | kZR);
        if (hx < -0.5f) g_pad_buttons.buttons |= kLeft;
        if (hx > 0.5f) g_pad_buttons.buttons |= kRight;
        if (hy < -0.5f) g_pad_buttons.buttons |= kUp;
        if (hy > 0.5f) g_pad_buttons.buttons |= kDown;
        // LTRIGGER/RTRIGGER are 0..1 on Android; the brake/gas axes are the same trigger on some pads.
        auto trigger = [&](int32_t main_axis, int32_t alt_axis) {
            const float v = std::max(AMotionEvent_getAxisValue(e, main_axis, 0),
                                     AMotionEvent_getAxisValue(e, alt_axis, 0));
            return pad_trigger_pressed(int32_t(std::clamp(v, 0.f, 1.f) * 32767.f), cfg.trigger);
        };
        if (trigger(AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_BRAKE)) g_pad_buttons.buttons |= kZL;
        if (trigger(AMOTION_EVENT_AXIS_RTRIGGER, AMOTION_EVENT_AXIS_GAS)) g_pad_buttons.buttons |= kZR;
        return 1;
    }
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return 0;
    if (!touch_enabled()) return 0;  // NX_TOUCH_SHOW=0
    int32_t action = AMotionEvent_getAction(e);
    int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    size_t index = size_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    if (ANativeWindow* w = android_window()) {
        g_w = float(ANativeWindow_getWidth(w));
        g_h = float(ANativeWindow_getHeight(w));
    }
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        float x = AMotionEvent_getX(e, index), y = AMotionEvent_getY(e, index);
        g_gamepad = false;
        size_t drawn = 0;
        touch_buttons(drawn);
        if (drawn) {  // a drawn layout (touch_layout.cpp): a button, else a stick where the layout allows one
            const float nx = x / g_w, ny = y / g_h;
            if (const uint64_t d = tz::dpad_hit(touch_dpad(), nx, ny, g_w / g_h)) {
                g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, false, false, d, true});
                return 1;
            }
            uint64_t b = touch_hit(nx, ny, g_w / g_h);
            const tz::StickArea move = touch_move_stick(), camera = touch_camera_stick();
            bool stick = false, is_camera = false;
            if (!b) {
                if (tz::stick_contains(move, nx, ny)) stick = true;
                else if (tz::stick_contains(camera, nx, ny)) stick = true, is_camera = true;
            }
            if (b || stick)
                g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, stick, is_camera, b});
        } else {
            bool left = x < g_w * 0.5f;
            g_touches.push_back({AMotionEvent_getPointerId(e, index), x, y, x, y, left, false,
                                 left ? 0 : touch_button(x, y)});
        }
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP ||
               kind == AMOTION_EVENT_ACTION_CANCEL) {
        int32_t id = AMotionEvent_getPointerId(e, index);
        if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_CANCEL) g_touches.clear();
        g_touches.erase(std::remove_if(g_touches.begin(), g_touches.end(), [&](const Touch& t) { return t.id == id; }),
                        g_touches.end());
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t k = 0; k < AMotionEvent_getPointerCount(e); k++) {
            int32_t id = AMotionEvent_getPointerId(e, k);
            for (auto& t : g_touches)
                if (t.id == id) {
                    t.x = AMotionEvent_getX(e, k);
                    t.y = AMotionEvent_getY(e, k);
                    if (t.dpad) t.button = tz::dpad_hit(touch_dpad(), t.x / g_w, t.y / g_h, g_w / g_h, true);
                }
        }
    }
    return 1;
}

bool headless() {  // NX_HEADLESS (platform.h)
    static const bool on = std::getenv("NX_HEADLESS") != nullptr;
    return on;
}

void read_pad(PadState& p) {
    std::lock_guard lk(g_in);
    p.buttons |= g_pad_buttons.buttons;
    if (g_pad_buttons.lx || g_pad_buttons.ly) {
        p.lx = g_pad_buttons.lx;
        p.ly = g_pad_buttons.ly;
    }
    p.rx = g_pad_buttons.rx;
    p.ry = g_pad_buttons.ry;
    float radius = std::max(1.f, g_h * 0.12f);
    size_t drawn = 0;
    touch_buttons(drawn);
    for (const auto& t : g_touches) {
        if (t.stick) {
            const float dx = (t.x - t.x0) / radius, dy = (t.y - t.y0) / radius;
            const int32_t sx = int32_t(std::clamp(dx, -1.f, 1.f) * kStickMax);
            const int32_t sy = int32_t(std::clamp(-dy, -1.f, 1.f) * kStickMax);
            if (t.camera) {  // the right thumb's floating stick (Bayonetta 3's camera)
                p.rx = sx;
                p.ry = sy;
            } else {
                p.lx = sx;
                p.ly = sy;
                // Wonder: a movement stick pushed nearly all the way holds Y too (dash), so running needs no second
                // thumb on the right side. Bayonetta 3 has no dash button, so its layout says no.
                if (drawn && touch_dash_full_tilt() && std::abs(dx) > 0.85f) p.buttons |= kY;
            }
        } else {
            p.buttons |= t.button;
        }
    }
    // The launcher's controller settings (config.txt NX_PAD_*), the same ones the desktop backends apply: face button
    // layout, camera inversion and the trigger threshold, over the pad and the touch controls alike.
    const PadConfig cfg = pad_config_from_env();
    if (cfg.swap_face) p.buttons = pad_swap_face(p.buttons);  // only when asked: the historical default is Nintendo
    pad_camera(p.rx, p.ry, cfg);
    input_script(p);  // NX_INPUT_SCRIPT (config.txt): scripted runs on the phone, as on the Mac
}

// The drawn layout's buttons, the D-pad and both sticks (touch_art.h), with motion (touch_layout.h Spring): a press
// dips the button and sends out a ripple; a stick's thumb trails the finger a little and springs back with a bounce
// when let go, while its range glides back to where the thumb usually lands. Stepped once per present, with the time
// since the last one. Nothing while a controller is in use or for games without a drawn layout.
size_t touch_sprites(OverlaySprite* out, size_t max) {
    size_t n = 0, k = 0;
    const TouchButton* b = touch_buttons(n);
    if (!n) return 0;
    art::prepare_atlas();  // drawn once, off this thread
    std::lock_guard lk(g_in);
    if ((g_gamepad && !touch_always()) || !touch_enabled()) return 0;
    static uint64_t last = 0;
    const uint64_t now = monotonic_ns();
    const float dt = last ? std::min(0.1f, float(now - last) * 1e-9f) : 0.0f;
    last = now;
    const float shade = touch_opacity_scale(), aspect = g_w / g_h, radius = 0.12f;

    const Touch* move = nullptr;
    const Touch* camera = nullptr;
    uint64_t held = 0, dpad = 0;
    for (const auto& t : g_touches) {
        if (t.stick && t.camera) camera = &t;
        else if (t.stick) move = &t;
        else if (t.dpad) dpad |= t.button;
        else held |= t.button;
    }
    // Buttons: a press spring and the time since the last press began, per button.
    constexpr size_t kMax = 24;
    static Spring press[kMax];
    static float ripple[kMax];
    static bool was[kMax];
    static bool init = false;
    if (!init) {
        for (float& r : ripple) r = -1.0f;
        init = true;
    }
    for (size_t i = 0; i < n && i < kMax; i++) {
        const bool down = (held & b[i].button) != 0;
        if (down && !was[i]) ripple[i] = 0.0f;
        was[i] = down;
        press[i].step(down ? 1.0f : 0.0f, dt, 900.0f, 34.0f);
        if (ripple[i] >= 0.0f) ripple[i] = ripple[i] + dt / 0.38f >= 1.0f ? -1.0f : ripple[i] + dt / 0.38f;
        k += button_sprites(b[i], press[i].x, ripple[i], shade, out + k, max - k);
    }
    // The D-pad's arms.
    static Spring arm[4];
    static constexpr uint64_t kArmBits[4] = {kUp, kRight, kDown, kLeft};
    float arms[4];
    for (int i = 0; i < 4; i++) arm[i].step((dpad & kArmBits[i]) ? 1.0f : 0.0f, dt, 900.0f, 40.0f), arms[i] = arm[i].x;
    k += dpad_sprites(touch_dpad(), arms, shade, out + k, max - k);
    // A stick: where its range sits (the touch-down point while held, else its resting place) and its thumb.
    struct Stick {
        Spring x, y, tx, ty, on;
        bool placed = false;
    };
    auto stick = [&](Stick& st, const Touch* t, bool is_camera, float r) {
        float rx = 0, ry = 0;
        touch_stick_rest(is_camera, rx, ry);
        if (!st.placed) st.x.x = rx, st.y.x = ry, st.placed = true;
        if (t) {  // the range jumps under the thumb (a floating stick); the thumb trails the finger
            st.x.x = t->x0 / g_w, st.y.x = t->y0 / g_h, st.x.v = st.y.v = 0.0f;
            const float dx = (t->x - t->x0) / g_h, dy = (t->y - t->y0) / g_h, len = std::hypot(dx, dy);
            const float s = len > r ? r / len : 1.0f;
            st.tx.step(dx * s, dt, 2400.0f, 80.0f);
            st.ty.step(dy * s, dt, 2400.0f, 80.0f);
        } else {  // let go: the thumb springs home with a bounce, the range glides back to rest
            st.tx.step(0.0f, dt, 700.0f, 20.0f);
            st.ty.step(0.0f, dt, 700.0f, 20.0f);
            st.x.step(rx, dt, 120.0f, 22.0f);
            st.y.step(ry, dt, 120.0f, 22.0f);
        }
        st.on.step(t ? 1.0f : 0.0f, dt, 500.0f, 40.0f);
        k += stick_sprites(st.x.x, st.y.x, st.tx.x, st.ty.x, r, st.on.x, aspect, shade * (0.9f + 0.1f * st.on.x),
                           out + k, max - k);
    };
    static Stick left, right;
    stick(left, move, false, radius);
    if (touch_camera_stick().valid) stick(right, camera, true, radius * 0.85f);
    return k;
}

}  // namespace nx::plat
