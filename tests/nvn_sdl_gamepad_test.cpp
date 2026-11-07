// Exercise the real SDL input and HID vibration paths with virtual pads only.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#define main nvn_unused_platform_main
#include "../runtime/src/platform_linux.cpp"
#undef main

namespace {
using namespace nx::plat;
void check(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (SDL: %s)\n", message, SDL_GetError());
        std::exit(1);
    }
}
std::map<std::string, nx::HleFn>& registry() {
    static std::map<std::string, nx::HleFn> value;
    return value;
}
uint64_t invoke(const char* suffix, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) {
    const auto name = std::string("_ZN2nn3hid") + suffix;
    auto it = registry().find(name);
    check(it != registry().end(), name.c_str());
    nx::CPU cpu{};
    cpu.x[0] = a; cpu.x[1] = b; cpu.x[2] = c; cpu.x[3] = d;
    it->second(cpu);
    return cpu.x[0];
}
void only_virtual_gamepads() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsJoystickVirtual(ids[i])) {
            SDL_free(ids);
            std::fprintf(stderr, "SKIP: physical gamepad visible; refusing to open or rumble it\n");
            std::exit(77);
        }
    }
    SDL_free(ids);
}
struct Event { Uint16 low, high; };
struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    std::vector<Event> events;
    static bool SDLCALL rumble(void* user, Uint16 low, Uint16 high) {
        static_cast<VirtualPad*>(user)->events.push_back({low, high});
        return true;
    }
    void attach() {
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0xfffe; desc.product_id = 0xeffe;
        desc.name = "Recomp virtual test pad";
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.userdata = this; desc.Rumble = rumble;
        id = SDL_AttachVirtualJoystick(&desc);
        check(id != 0, "attach virtual gamepad");
        joystick = SDL_OpenJoystick(id);
        check(joystick != nullptr, "open virtual joystick");
        axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
        SDL_UpdateJoysticks();
        only_virtual_gamepads();
        check(SDL_IsGamepad(id), "virtual joystick has gamepad mapping");
    }
    void detach() {
        check(SDL_DetachVirtualJoystick(id), "detach virtual joystick");
        SDL_CloseJoystick(joystick);
        joystick = nullptr; id = 0;
        SDL_UpdateJoysticks();
    }
    void button(SDL_GamepadButton b, bool pressed) {
        check(SDL_SetJoystickVirtualButton(joystick, int(b), pressed), "set virtual button");
    }
    void axis(SDL_GamepadAxis a, Sint16 value) {
        check(SDL_SetJoystickVirtualAxis(joystick, int(a), value), "set virtual axis");
    }
};
PadState sample() {
    SDL_UpdateJoysticks();
    only_virtual_gamepads();
    nx::plat::sample_input();
    PadState p;
    nx::plat::read_pad(p);
    return p;
}
void check_button(VirtualPad& pad, SDL_GamepadButton button, uint64_t expected) {
    pad.button(button, true);
    check(sample().buttons == expected, "individual button mapping");
    pad.button(button, false);
    check(sample().buttons == 0, "button release clears bit");
}
void controls(bool swap, bool invert) {
    VirtualPad pad;
    pad.attach();
    check(sample().buttons == 0, "idle pad has no pressed buttons");
    const std::array<std::pair<SDL_GamepadButton, uint64_t>, 14> buttons = {{
        {SDL_GAMEPAD_BUTTON_EAST, swap ? kB : kA}, {SDL_GAMEPAD_BUTTON_SOUTH, swap ? kA : kB},
        {SDL_GAMEPAD_BUTTON_NORTH, swap ? kY : kX}, {SDL_GAMEPAD_BUTTON_WEST, swap ? kX : kY},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR},
        {SDL_GAMEPAD_BUTTON_START, kPlus}, {SDL_GAMEPAD_BUTTON_BACK, kMinus},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kStickR},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
    }};
    for (const auto& [button, bit] : buttons) check_button(pad, button, bit);
    pad.button(SDL_GAMEPAD_BUTTON_EAST, true);
    pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    check(sample().buttons == ((swap ? kB : kA) | kUp), "multiple buttons together");
    pad.button(SDL_GAMEPAD_BUTTON_EAST, false);
    pad.button(SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 7848);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -7848);
    PadState p = sample();
    check(p.lx == 0 && p.ly == 0, "small stick inputs in deadzone");
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 32767);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTX, -32768);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTY, 32767);
    p = sample();
    check(p.lx == kStickMax && p.ly == kStickMax, "left stick limits and Y inversion");
    check(p.rx == (invert ? kStickMax : -kStickMax) && p.ry == (invert ? kStickMax : -kStickMax),
          "right stick limits and camera inversion");
    pad.axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
    pad.axis(SDL_GAMEPAD_AXIS_LEFTY, 0);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTX, 0);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHTY, 0);
    p = sample();
    check(p.lx == 0 && p.ly == 0 && p.rx == 0 && p.ry == 0, "stick release");
    pad.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    check(sample().buttons == (kZL | kZR), "full triggers press ZL and ZR");
    pad.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    check(sample().buttons == 0, "idle triggers release ZL and ZR");
    pad.detach();
    check(sample().buttons == 0, "disconnected controller is closed");
}
void trigger_boundaries(bool maximum) {
    VirtualPad pad;
    pad.attach();
    check(sample().buttons == 0, "idle trigger at boundary threshold");
    pad.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, maximum ? 32000 : 0);
    check(sample().buttons == (maximum ? 0 : kZL), "partial trigger at boundary threshold");
    pad.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    check(sample().buttons == (kZL | kZR), "maximum trigger reaches boundary threshold");
    pad.detach();
    check(sample().buttons == 0, "disconnected trigger controller is closed");
}
void selected_pad() {
    VirtualPad first, selected, replacement;
    first.attach();
    first.button(SDL_GAMEPAD_BUTTON_EAST, true);
    check(sample().buttons == 0, "missing selected index does not fall back");
    selected.attach();
    selected.button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    check(sample().buttons == kB, "late selected controller is picked up");
    selected.detach();
    check(sample().buttons == 0, "disconnect clears selected input");
    check(sample().buttons == 0, "disconnected selection does not fall back");
    replacement.attach();
    replacement.button(SDL_GAMEPAD_BUTTON_NORTH, true);
    check(sample().buttons == kX, "selected index reconnects");
    replacement.detach();
    check(sample().buttons == 0, "reconnected controller is closed before its callback data expires");
    first.detach();
}
void test_rumble() {
    VirtualPad pad, replacement;
    pad.attach();
    sample();
    uint32_t id = 0;
    std::array<uint32_t, 2> handles{};
    auto ptr = [](const void* p) { return reinterpret_cast<uint64_t>(p); };
    check(invoke("25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
                 ptr(handles.data()), 2, ptr(&id), 1) == 2, "HID returns two vibration handles");
    struct Value { float low, low_frequency, high, high_frequency; };
    auto send = [&](int which, float low, float high) {
        const Value value{low, 160.0f, high, 320.0f};
        invoke("18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE",
               ptr(&handles[size_t(which)]), ptr(&value));
    };
    const char* batch = "6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi";
    std::array<Value, 2> values{{{.25f, 160.0f, .75f, 320.0f}, {.5f, 160.0f, 0.0f, 320.0f}}};
    check(invoke(batch, ptr(handles.data()), ptr(values.data()), values.size()) == 0, "batch HID send succeeds");
    check(pad.events.empty(), "HID publishes only; SDL runs on sampling thread");
    sample();
    check(!pad.events.empty() && pad.events.back().low == 49151 && pad.events.back().high == 32768,
          "batch HID amplitudes reach both SDL motors");
    values = {{{0, 160, 0, 320}, {0, 160, 0, 320}}};
    check(invoke(batch, ptr(handles.data()), ptr(values.data()), values.size()) == 0, "batch HID stop succeeds");
    sample();
    check(pad.events.back().low == 0 && pad.events.back().high == 0, "batch stop reaches both SDL motors");
    send(0, .25f, .75f);
    send(1, .5f, 0.0f);
    sample();
    check(pad.events.back().low == 49151 && pad.events.back().high == 32768,
          "single HID sends reach both SDL motors");
    send(0, 0.0f, 0.0f);
    sample();
    check(pad.events.back().low == 0 && pad.events.back().high == 32768,
          "stopping left actuator preserves right actuator");
    send(0, 1.0f, .2f);
    sample();
    check(pad.events.back().low == 65535 && pad.events.back().high == 32768, "changed intensity reaches SDL");
    const size_t before_refresh = pad.events.size();
    SDL_Delay(270);
    sample();
    // SDL may suppress a callback for unchanged strengths, while extending the
    // effect expiry. Cross the original 500ms expiry and check it never stopped.
    SDL_Delay(270);
    sample();
    for (size_t i = before_refresh; i < pad.events.size(); ++i)
        check(pad.events[i].low != 0, "refreshed rumble does not expire");
    check(pad.events.back().low == 65535 && pad.events.back().high == 32768, "nonzero rumble sustained");
    send(0, 0.0f, 0.0f);
    send(1, 0.0f, 0.0f);
    sample();
    check(pad.events.back().low == 0 && pad.events.back().high == 0, "HID stop reaches SDL");
    send(0, .5f, 0.0f);
    sample();
    check(pad.events.back().low == 32768, "rumble restarts before disconnect");
    pad.detach();
    check(sample().buttons == 0, "disconnect clears input while rumbling");
    replacement.attach();
    sample();
    check(replacement.events.empty() || (replacement.events.back().low == 0 && replacement.events.back().high == 0),
          "old rumble is not replayed on the new pad");
    send(1, .5f, 0.0f);
    sample();
    check(!replacement.events.empty() && replacement.events.back().low == 0 && replacement.events.back().high == 32768,
          "new right rumble does not resurrect old left rumble");
    invoke("14DisconnectNpadERKj", ptr(&id));
    sample();
    check(replacement.events.back().low == 0 && replacement.events.back().high == 0,
          "HID disconnect stops host rumble");
    replacement.detach();
    check(sample().buttons == 0, "reconnected rumble controller is closed");
}
}
namespace nx {
std::atomic<uint64_t> g_frame{0};
HleRegistrar::HleRegistrar(const char* name, HleFn fn) { check(registry().emplace(name, fn).second, "unique HLE registrations"); }
void log(const char*, ...) {}
uint64_t find_symbol(const char*) { std::abort(); }
void call(CPU&, uint64_t) { std::abort(); }
namespace plat { void input_script(PadState&) {} }
}
int main(int argc, char** argv) {
    check(argc == 2, "scenario argument");
    // Do not open or rumble physical controllers, even if SDL ignores the hint.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0xfffe/0xeffe");
    check(SDL_Init(SDL_INIT_GAMEPAD), "initialize SDL gamepads");
    only_virtual_gamepads();
    const std::string mode = argv[1];
    if (mode == "default") controls(false, false);
    else if (mode == "swap") controls(true, true);
    else if (mode == "trigger-zero") trigger_boundaries(false);
    else if (mode == "trigger-max") trigger_boundaries(true);
    else if (mode == "selection") selected_pad();
    else if (mode == "rumble") test_rumble();
    else check(false, "known scenario");
    SDL_Quit();
    std::printf("nvn_sdl_gamepad_test: %s passed\n", mode.c_str());
}
