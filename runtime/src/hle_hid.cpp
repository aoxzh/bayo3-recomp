// nn::hid API HLE: one FullKey controller backed by the platform keyboard/controller.
// Raw touch, mouse, and motion hardware are not exposed by platform.h. Their state is
// explicitly empty/disconnected; Android touch buttons still feed the FullKey pad.
// Guest ABI sizes were checked against B3's SDK NSO; see docs/BAYONETTA3_HID.md.
#include <array>
#include <atomic>
#include <deque>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "platform.h"
#include "pad_rumble.h"

namespace nx {

namespace {

using namespace plat;

constexpr uint32_t kStyleFullKey = 1u << 0;
std::atomic<uint64_t> g_sampling{0};
constexpr size_t kNpadStateSize = 0x28;
constexpr size_t kMouseStateSize = 0x28;
constexpr size_t kSixAxisStateSize = 0x60;
constexpr size_t kTouchHeaderSize = 0x10;
constexpr size_t kTouchPointSize = 0x28;

struct VibrationAmplitudes {
    uint16_t low = 0;
    uint16_t high = 0;
};

struct HidState {
    std::mutex mutex;
    uint32_t styles = kStyleFullKey;
    std::vector<uint32_t> ids{0};
    uint32_t hold = 0;
    uint32_t activation = 0;
    bool disconnected = false;
    uint32_t vibration_generation = 0;
    std::array<VibrationAmplitudes, 2> vibration{};
    std::deque<std::array<uint8_t, kNpadStateSize>> history;
};
HidState g_hid;

// Caller holds g_hid.mutex. The host API supplies player 1 only; unsupported
// IDs/styles never acquire a connection simply because the game asks for them.
bool connected(uint32_t id) {
    return id == 0 && !g_hid.disconnected && (g_hid.styles & kStyleFullKey) &&
           std::find(g_hid.ids.begin(), g_hid.ids.end(), id) != g_hid.ids.end();
}

// Caller holds g_hid.mutex. A physical device change invalidates both cached
// actuator values; otherwise sending one side could restore the other side.
void sync_vibration_generation() {
    const uint32_t generation = pad_rumble_generation();
    if (generation != g_hid.vibration_generation) {
        g_hid.vibration_generation = generation;
        g_hid.vibration = {};
    }
}

void publish_vibration() {
    const auto& left = g_hid.vibration[0];
    const auto& right = g_hid.vibration[1];
    if (!try_publish_pad_rumble(g_hid.vibration_generation, std::max(left.low, left.high),
                                std::max(right.low, right.high))) {
        // A close/open won the race. Drop the old values instead of publishing
        // them into the new device's generation.
        g_hid.vibration = {};
        g_hid.vibration_generation = pad_rumble_generation();
    }
}

// Caller holds g_hid.mutex, including during publication, so an older send
// cannot restart rumble after a concurrent disconnect or configuration change.
void clear_vibration() {
    sync_vibration_generation();
    g_hid.vibration = {};
    publish_vibration();
}

void SetSupportedNpadStyleSet(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    g_hid.styles = uint32_t(c.x[0]);
    g_hid.history.clear();
    if (!connected(0)) clear_vibration();
}
void GetSupportedNpadStyleSet(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    c.x[0] = g_hid.styles;
}
void SetSupportedNpadIdType(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    g_hid.ids.clear();
    for (uint64_t i = 0; i < c.x[1]; ++i) g_hid.ids.push_back(ld<uint32_t>(c.x[0] + i * 4));
    g_hid.history.clear();
    if (!connected(0)) clear_vibration();
}
void SetNpadJoyHoldType(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    g_hid.hold = uint32_t(c.x[0]);
}
void GetNpadJoyHoldType(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    c.x[0] = g_hid.hold;
}
void SetNpadHandheldActivationMode(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    g_hid.activation = uint32_t(c.x[0]);
}
void GetNpadHandheldActivationMode(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    c.x[0] = g_hid.activation;
}
void DisconnectNpad(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    if (ld<uint32_t>(c.x[0]) == 0) {
        g_hid.disconnected = true;
        g_hid.history.clear();
        clear_vibration();
    }
}

// Scripted input for automated runs: NX_INPUT="3300:A,3900:A,4200:Down" holds the button for 8 frames from the
// given presented frame.
struct Scripted {
    uint64_t frame;
    uint64_t buttons;
};

std::vector<Scripted> input_script() {
    std::vector<Scripted> v;
    const char* e = std::getenv("NX_INPUT");
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            uint64_t bits = b == "A" ? kA : b == "B" ? kB : b == "X" ? kX : b == "Y" ? kY : b == "Plus" ? kPlus :
                            b == "Minus" ? kMinus : b == "Up" ? kUp : b == "Down" ? kDown : b == "Left" ? kLeft :
                            b == "Right" ? kRight : b == "L" ? kL : b == "R" ? kR : 0;
            v.push_back({std::strtoull(item.c_str(), nullptr, 10), bits});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

PadState read() {
    PadState p;
    static const std::vector<Scripted> script = input_script();
    for (const auto& s : script)
        if (g_frame >= s.frame && g_frame < s.frame + 8) p.buttons |= s.buttons;
    read_pad(p);
    // Stick directions as buttons, like the hid sysmodule reports them.
    constexpr int32_t t = kStickMax / 2;
    if (p.lx < -t) p.buttons |= kStickLLeft;
    if (p.lx > t) p.buttons |= kStickLRight;
    if (p.ly > t) p.buttons |= kStickLUp;
    if (p.ly < -t) p.buttons |= kStickLDown;
    if (p.rx < -t) p.buttons |= kStickRLeft;
    if (p.rx > t) p.buttons |= kStickRRight;
    if (p.ry > t) p.buttons |= kStickRUp;
    if (p.ry < -t) p.buttons |= kStickRDown;
    return p;
}

void empty_sample(uint64_t out, size_t size, size_t sampling_offset = 0) {
    std::memset(reinterpret_cast<void*>(out), 0, size);
    st<uint64_t>(out + sampling_offset, ++g_sampling);
}

// All five B3 Npad variants use the same 0x28-byte ABI, including JoyLeft/Right.
// Caller holds g_hid.mutex.
void npad_sample(uint64_t out, uint32_t id, bool full_key) {
    empty_sample(out, kNpadStateSize);
    if (!full_key || !connected(id)) return;
    PadState p = read();
    st<uint64_t>(out + 8, p.buttons);
    st<int32_t>(out + 16, p.lx);
    st<int32_t>(out + 20, p.ly);
    st<int32_t>(out + 24, p.rx);
    st<int32_t>(out + 28, p.ry);
    st<uint32_t>(out + 32, 1);  // IsConnected
}
void GetNpadState(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    npad_sample(c.x[0], ld<uint32_t>(c.x[1]), true);
}
void GetNpadStateOther(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    npad_sample(c.x[0], ld<uint32_t>(c.x[1]), false);
}

// Preserve newest-first FullKey history for SDK clients using the plural API.
void GetNpadStatesFullKey(CPU& c) {
    if (int32_t(c.x[1]) < 1) { c.x[0] = 0; return; }
    std::lock_guard lock(g_hid.mutex);
    uint64_t out = c.x[0];
    uint32_t id = ld<uint32_t>(c.x[2]);
    npad_sample(out, id, true);
    if (!connected(id)) { c.x[0] = 1; return; }
    std::array<uint8_t, kNpadStateSize> now;
    std::memcpy(now.data(), reinterpret_cast<const void*>(out), now.size());
    g_hid.history.push_front(now);
    if (g_hid.history.size() > 16) g_hid.history.pop_back();
    int count = std::min<int>(int32_t(c.x[1]), int(g_hid.history.size()));
    for (int k = 1; k < count; k++)
        std::memcpy(reinterpret_cast<void*>(out + kNpadStateSize * k), g_hid.history[k].data(), kNpadStateSize);
    c.x[0] = uint64_t(count);
}
void GetNpadStatesOther(CPU& c) {
    if (int32_t(c.x[1]) < 1) { c.x[0] = 0; return; }
    empty_sample(c.x[0], kNpadStateSize);
    c.x[0] = 1;
}

void GetMouseState(CPU& c) { empty_sample(c.x[0], kMouseStateSize); }
void GetMouseStates(CPU& c) {
    if (int32_t(c.x[1]) < 1) { c.x[0] = 0; return; }
    GetMouseState(c);
    c.x[0] = 1;
}
void GetKeyboardState(CPU& c) { empty_sample(c.x[0], 0x30); }
void GetTouchScreenState1(CPU& c) { empty_sample(c.x[0], kTouchHeaderSize + kTouchPointSize); }
void GetTouchScreenState16(CPU& c) { empty_sample(c.x[0], kTouchHeaderSize + 16 * kTouchPointSize); }
// int GetTouchScreenStates<1>(TouchScreenState<1>* out, int count): program 1 (the Bayonetta Origins prologue)
// imports the plural form. Left to the SDK it reads the touch lifo through the shared-memory accessor that the
// InitializeTouchScreen HLE never activated, and faults at 0x18.
void GetTouchScreenStates1(CPU& c) {
    if (int32_t(c.x[1]) < 1) { c.x[0] = 0; return; }
    GetTouchScreenState1(c);
    c.x[0] = 1;
}
// SixAxisSensorState: deltaTime +0, sampling +8, acceleration +0x10,
// angularVelocity +0x1c, angle +0x28, direction +0x34, attributes +0x58.
// No sensor is exposed. Never supply gravity, identity orientation, or connection bits.
void GetSixAxisSensorState(CPU& c) { empty_sample(c.x[0], kSixAxisStateSize, 8); }
void GetSixAxisSensorStates(CPU& c) {
    if (int32_t(c.x[1]) < 1) { c.x[0] = 0; return; }
    GetSixAxisSensorState(c);
    c.x[0] = 1;  // one explicitly disconnected sample, not a device handle
}

void zero(CPU& c) { c.x[0] = 0; }
void one(CPU& c) { c.x[0] = 1; }

void GetNpadStyleSet(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    c.x[0] = connected(ld<uint32_t>(c.x[0])) ? kStyleFullKey : 0;
}

void BindNpadStyleSetUpdateEvent(CPU& c) {  // (const u32& id, SystemEventType*, EventClearMode)
    uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t ev = c.x[1], mode = c.x[2];
    uint32_t id = ld<uint32_t>(c.x[0]);
    c.x[0] = ev;
    c.x[1] = mode;
    c.x[2] = 0;
    call(c, create);
    // Player 1's controller is there from the start: the style set "changed" (connected) once, and games wait for
    // this event before they look at the npad.
    static uint64_t signal = find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (signal) {
        c.x[0] = ev;
        call(c, signal);
    }
    log("hid: style set event bound for id %#x, signalled", id);
}

// A vibration handle is four bytes: style index, player, actuator index and
// padding. FullKey/No1 has two actuators. Switchbrew documents the structures:
// https://switchbrew.org/wiki/HID_services#VibrationDeviceHandle
// https://switchbrew.org/wiki/HID_services#VibrationValue
constexpr uint32_t kVibrationFullKey = 3;
constexpr uint32_t kVibrationRight = kVibrationFullKey | (1u << 16);

// Caller holds g_hid.mutex. Reject nonzero padding and any non-FullKey/No1 handle.
int vibration_actuator(uint64_t handle_ptr) {
    if (!handle_ptr || !connected(0)) return -1;
    const uint32_t handle = ld<uint32_t>(handle_ptr);
    if (handle == kVibrationFullKey) return 0;
    if (handle == kVibrationRight) return 1;
    return -1;
}

void GetVibrationDeviceHandles(CPU& c) {
    const uint64_t out = c.x[0];
    const int32_t capacity = int32_t(c.x[1]);
    const uint64_t id_ptr = c.x[2];
    const uint32_t style = uint32_t(c.x[3]);
    c.x[0] = 0;
    // The requested style is a single style, not the supported-style mask.
    if (!out || !id_ptr || capacity <= 0 || style != kStyleFullKey) return;
    std::lock_guard lock(g_hid.mutex);
    if (!connected(ld<uint32_t>(id_ptr))) return;
    st<uint32_t>(out, kVibrationFullKey);
    if (capacity > 1) st<uint32_t>(out + 4, kVibrationRight);
    c.x[0] = capacity > 1 ? 2 : 1;
}

void GetVibrationDeviceInfo(CPU& c) {
    const uint64_t out = c.x[0];
    if (!out) return;
    std::lock_guard lock(g_hid.mutex);
    const int actuator = vibration_actuator(c.x[1]);
    // Unknown/None for an unsupported or disconnected device.
    st<uint32_t>(out, actuator < 0 ? 0 : 1);       // LinearResonantActuator
    st<uint32_t>(out + 4, actuator < 0 ? 0 : uint32_t(actuator + 1));  // Left/Right
}

uint16_t vibration_amplitude(float value) {
    if (!std::isfinite(value) || value <= 0.0f) return 0;
    if (value >= 1.0f) return UINT16_MAX;
    return uint16_t(value * 65535.0f + 0.5f);
}

// Caller holds g_hid.mutex. SDL cannot represent frequency, so retain each
// actuator's amplitudes and publish its stronger band as its motor strength.
void update_vibration(int actuator, uint64_t value) {
    auto& v = g_hid.vibration[size_t(actuator)];
    v.low = vibration_amplitude(ld<float>(value));
    v.high = vibration_amplitude(ld<float>(value + 8));
}

void SendVibrationValue(CPU& c) {
    std::lock_guard lock(g_hid.mutex);
    sync_vibration_generation();
    const int actuator = vibration_actuator(c.x[0]);
    if (actuator < 0 || !c.x[1]) return;
    update_vibration(actuator, c.x[1]);
    publish_vibration();
}

void SendVibrationValues(CPU& c) {
    const uint64_t handles = c.x[0], values = c.x[1];
    const int32_t count = int32_t(uint32_t(c.x[2]));
    c.x[0] = 0;  // nn::Result success
    if (count <= 0 || !handles || !values) return;
    const uint64_t n = uint32_t(count);
    // Guest pointers are host addresses. Reject wraparound before the first load;
    // as elsewhere in the HLE, valid mapped input remains the caller's contract.
    if (handles > UINT64_MAX - (n * 4 - 1) || values > UINT64_MAX - (n * 16 - 1)) return;

    std::lock_guard lock(g_hid.mutex);
    sync_vibration_generation();
    bool changed = false;
    for (uint64_t i = 0; i < n; ++i) {
        const int actuator = vibration_actuator(handles + i * 4);
        if (actuator < 0) continue;
        update_vibration(actuator, values + i * 16);
        changed = true;
    }
    if (changed) publish_vibration();
}
// SDK controller-support failure result (sdk+26eec0). No applet is launched.
constexpr uint32_t kControllerSupportUnavailable = 0x183aca;
uint32_t controller_support(uint64_t arg, uint64_t out) {
    std::lock_guard lock(g_hid.mutex);
    if (out) std::memset(reinterpret_cast<void*>(out), 0, 8);
    // Arg begins with signed min/max player counts. Respect the configured style
    // and ID list; a request for two players cannot be satisfied by this host API.
    bool available = connected(0) && ld<int8_t>(arg) <= 1 && ld<int8_t>(arg + 1) >= 1;
    if (!available) return kControllerSupportUnavailable;
    if (out) {
        st<int8_t>(out, 1);
        st<uint32_t>(out + 4, 0);  // No1
    }
    return 0;
}
void ShowControllerSupport(CPU& c) { c.x[0] = controller_support(c.x[1], c.x[0]); }
void ShowControllerSupportNoResult(CPU& c) { c.x[0] = controller_support(c.x[0], 0); }
void noop(CPU& c) { c.x[0] = 0; }

}  // namespace

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", SetSupportedNpadStyleSet);
NX_HLE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", GetSupportedNpadStyleSet);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", SetNpadJoyHoldType);
NX_HLE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", GetNpadJoyHoldType);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", SetSupportedNpadIdType);
NX_HLE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", SetNpadHandheldActivationMode);
NX_HLE("_ZN2nn3hid29GetNpadHandheldActivationModeEv", GetNpadHandheldActivationMode);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadStateOther);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadStateOther);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", GetNpadStateOther);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", GetNpadStateOther);
NX_HLE("_ZN2nn3hid21InitializeTouchScreenEv", noop);
NX_HLE("_ZN2nn3hid19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE", GetTouchScreenState1);
NX_HLE("_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE", GetTouchScreenState16);
NX_HLE("_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi", GetTouchScreenStates1);
NX_HLE("_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE", GetMouseState);
NX_HLE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", GetSixAxisSensorState);
NX_HLE("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", ShowControllerSupportNoResult);
NX_HLE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
NX_HLE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", GetVibrationDeviceInfo);
NX_HLE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", ShowControllerSupport);
// SDK 16 (Super Mario Bros. Wonder)
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", GetNpadStatesFullKey);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", GetNpadStatesOther);
NX_HLE("_ZN2nn3hid15InitializeMouseEv", noop);
NX_HLE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", GetMouseStates);
NX_HLE("_ZN2nn3hid18InitializeKeyboardEv", noop);
NX_HLE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", GetKeyboardState);
NX_HLE("_ZN2nn3hid14DisconnectNpadERKj", DisconnectNpad);
NX_HLE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", noop);
NX_HLE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", noop);
NX_HLE("_ZN2nn3hid21StartLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20StopLrAssignmentModeEv", noop);
NX_HLE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", zero);  // dual
NX_HLE("_ZN2nn3hid20IsVibrationPermittedEv", one);
NX_HLE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", SendVibrationValue);
// B3 SDK's FlushEnqueuedVibrationValues calls PLT +0x5bfb30 through its
// R_AARCH64_JUMP_SLOT at +0xcdb068 for this exact symbol.
NX_HLE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", SendVibrationValues);
NX_HLE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", noop);
NX_HLE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", GetSixAxisSensorStates);

}  // namespace nx
