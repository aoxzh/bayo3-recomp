// Standalone API/ABI tests. The real hle_hid.cpp registrations are exercised;
// host input and guest calls are replaced, so this never starts the game.
#include <nx/runtime.h>
#include "../runtime/src/platform.h"
#include "../runtime/src/pad_rumble.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <thread>
#include <vector>

namespace {
std::map<std::string, nx::HleFn>& registry() {
    static std::map<std::string, nx::HleFn> value;
    return value;
}
nx::plat::PadState host_pad;
int host_reads = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
uint64_t invoke(const std::string& suffix, uint64_t a = 0, uint64_t b = 0, uint64_t d = 0, uint64_t e = 0) {
    const auto name = "_ZN2nn3hid" + suffix;
    auto it = registry().find(name);
    check(it != registry().end(), name.c_str());
    nx::CPU c{};
    c.x[0] = a; c.x[1] = b; c.x[2] = d; c.x[3] = e;
    it->second(c);
    return c.x[0];
}
struct Output {
    size_t size;
    std::vector<uint8_t> bytes;
    explicit Output(size_t n) : size(n), bytes(n + 32, 0xa5) {}
    uint64_t ptr() { return reinterpret_cast<uint64_t>(bytes.data() + 16); }
    void guards() {
        check(std::all_of(bytes.begin(), bytes.begin() + 16, [](auto b) { return b == 0xa5; }), "prefix sentinel");
        check(std::all_of(bytes.end() - 16, bytes.end(), [](auto b) { return b == 0xa5; }), "suffix sentinel");
    }
    void empty(size_t sampling = 0) {
        guards();
        check(nx::ld<uint64_t>(ptr() + sampling) > 0, "sampling is initialized");
        for (size_t i = 0; i < size; ++i)
            if (i < sampling || i >= sampling + 8) check(bytes[i + 16] == 0, "empty output fully initialized");
    }
    void untouched() { check(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == 0xa5; }), "zero count writes nothing"); }
};

constexpr const char* get_vibration_handles = "25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE";
constexpr const char* init_vibration = "25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE";
constexpr const char* get_vibration_info = "22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE";
constexpr const char* send_vibration = "18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE";
constexpr const char* send_vibration_values = "6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi";
constexpr const char* set_styles = "24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE";
constexpr const char* set_ids = "22SetSupportedNpadIdTypeEPKjm";

struct VibrationValue { float low, low_frequency, high, high_frequency; };
static_assert(sizeof(VibrationValue) == 16);
uint32_t rumble() { return nx::plat::read_pad_rumble(); }
uint32_t motors(uint16_t left, uint16_t right) { return uint32_t(left) | (uint32_t(right) << 16); }
void send(uint32_t& handle, float low, float high) {
    VibrationValue value{low, 160.0f, high, 320.0f};
    invoke(send_vibration, reinterpret_cast<uint64_t>(&handle), reinterpret_cast<uint64_t>(&value));
}

void test_vibration(uint32_t& id) {
    const auto idref = reinterpret_cast<uint64_t>(&id);
    uint32_t left = 3, right = 0x10003;
    const auto left_ref = reinterpret_cast<uint64_t>(&left);
    const auto right_ref = reinterpret_cast<uint64_t>(&right);
    check(rumble() == 0, "vibration starts silent");
    check(invoke("20IsVibrationPermittedEv") == 1, "vibration is permitted");
    for (uint64_t capacity : {0ull, 0xffffffffull}) {
        Output out(8);
        check(invoke(get_vibration_handles, out.ptr(), capacity, idref, 1) == 0, "nonpositive vibration capacity");
        out.untouched();
    }
    check(invoke(get_vibration_handles, 0, 2, idref, 1) == 0, "null vibration output");
    check(invoke(get_vibration_handles, 0, 0, 0, 1) == 0, "empty capacity avoids null input");
    for (int capacity : {1, 2, 3}) {
        Output out(size_t(capacity) * 4);
        const uint64_t count = invoke(get_vibration_handles, out.ptr(), uint64_t(capacity), idref, 1);
        check(count == uint64_t(std::min(capacity, 2)), "bounded FullKey vibration count");
        check(nx::ld<uint32_t>(out.ptr()) == left, "left FullKey vibration handle");
        if (capacity > 1) check(nx::ld<uint32_t>(out.ptr() + 4) == right, "right FullKey vibration handle");
        if (capacity > 2) check(nx::ld<uint32_t>(out.ptr() + 8) == 0xa5a5a5a5u, "unused handle untouched");
        out.guards();
    }
    for (uint32_t style : {0u, 2u, 4u, 8u, 3u, 5u, 7u, 0x80000000u, 0x80000001u, 0xffffffffu}) {
        Output out(8);
        check(invoke(get_vibration_handles, out.ptr(), 2, idref, style) == 0, "unsupported vibration style");
        out.untouched();
    }
    for (uint32_t other : {1u, 7u, 0x10u, 0x20u, 0xffffffffu}) {
        Output out(8);
        check(invoke(get_vibration_handles, out.ptr(), 2, reinterpret_cast<uint64_t>(&other), 1) == 0, "unsupported vibration player");
        out.untouched();
    }
    invoke(init_vibration, left_ref);
    invoke(init_vibration, right_ref);
    for (int index = 0; index < 2; ++index) {
        Output info(8);
        invoke(get_vibration_info, info.ptr(), index == 0 ? left_ref : right_ref);
        check(nx::ld<uint32_t>(info.ptr()) == 1, "vibration is a linear resonant actuator");
        check(nx::ld<uint32_t>(info.ptr() + 4) == uint32_t(index + 1), "vibration position matches actuator");
        info.guards();
    }
    invoke(get_vibration_info, 0, left_ref); // null output is ignored

    send(left, 0.25f, 0.75f);
    check(rumble() == motors(49151, 0), "left actuator uses its stronger frequency band");
    send(right, 0.25f, 0.125f);
    check(rumble() == motors(49151, 16384), "right actuator is independent");
    send(left, 0.0f, 0.0f);
    check(rumble() == motors(0, 16384), "stopping left preserves right");
    send(left, 0.5f, 0.125f);
    check(rumble() == motors(32768, 16384), "left can be updated independently");
    for (uint32_t invalid : {0u, 4u, 0x103u, 0x20003u, 0x1000003u, 0xffffffffu}) {
        Output info(8);
        invoke(get_vibration_info, info.ptr(), reinterpret_cast<uint64_t>(&invalid));
        check(nx::ld<uint64_t>(info.ptr()) == 0, "invalid handle reports unknown position and type");
        info.guards();
        invoke(init_vibration, reinterpret_cast<uint64_t>(&invalid));
        send(invalid, 1.0f, 1.0f);
        check(rumble() == motors(32768, 16384), "invalid handle cannot change motors");
    }
    Output null_info(8);
    invoke(get_vibration_info, null_info.ptr(), 0);
    check(nx::ld<uint64_t>(null_info.ptr()) == 0, "null handle returns unknown info");
    null_info.guards();
    invoke(send_vibration, 0, 0);
    invoke(send_vibration, left_ref, 0);
    check(rumble() == motors(32768, 16384), "null send arguments leave motors unchanged");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    send(left, nan, -inf);
    send(right, inf, -0.5f);
    check(rumble() == 0, "NaN, infinity and negative amplitudes are silent");
    send(left, -1.0f, 2.0f);
    send(right, std::numeric_limits<float>::max(), nan);
    check(rumble() == motors(65535, 65535), "out-of-range finite amplitudes clamp to one");
    send(left, 0, 0);
    send(right, 0, 0);
    check(rumble() == 0, "both zero values stop rumble");

    send(left, 0.25f, 0.75f);
    send(right, 0.5f, 0);
    invoke(set_styles, 7);  // FullKey remains enabled
    check(rumble() == motors(49151, 32768), "retaining FullKey preserves rumble");
    Output mixed_config(8);
    check(invoke(get_vibration_handles, mixed_config.ptr(), 2, idref, 1) == 2, "single FullKey style works with multi-style configuration");
    check(nx::ld<uint32_t>(mixed_config.ptr()) == left && nx::ld<uint32_t>(mixed_config.ptr() + 4) == right, "configured FullKey handle pair");
    mixed_config.guards();
    invoke(set_styles, 6);
    check(rumble() == 0, "filtering FullKey stops both actuators");
    send(left, 1, 1);
    check(rumble() == 0, "filtered FullKey cannot restart rumble");
    Output filtered(8);
    check(invoke(get_vibration_handles, filtered.ptr(), 2, idref, 1) == 0, "filtered FullKey exposes no handles");
    filtered.untouched();
    invoke(set_styles, 7);
    send(right, 0.5f, 0);
    check(rumble() == motors(0, 32768), "re-enabling does not restore stale left actuator");
    uint32_t other = 1;
    invoke(set_ids, reinterpret_cast<uint64_t>(&other), 1);
    check(rumble() == 0, "filtering player No1 stops rumble");
    send(right, 1, 1);
    check(rumble() == 0, "filtered player cannot restart rumble");
    invoke(set_ids, idref, 1);
    send(left, 0.5f, 0);
    check(rumble() == motors(32768, 0), "restoring player does not revive stale right actuator");
    send(left, 0, 0);

    // Concurrent real registered sends must publish coherent 32-bit snapshots
    // and retain both actuators' latest writes rather than lose one update.
    std::atomic<bool> start{false};
    std::atomic<int> done{0};
    auto writer = [&](uint32_t& handle, bool high_band) {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 20000; ++i)
            send(handle, high_band ? 0.0f : 1.0f, high_band ? 1.0f : 0.0f);
        done.fetch_add(1, std::memory_order_release);
    };
    std::thread a(writer, std::ref(left), false);
    std::thread b(writer, std::ref(right), true);
    start.store(true, std::memory_order_release);
    while (done.load(std::memory_order_acquire) != 2) {
        const uint32_t snapshot = rumble();
        const uint16_t l = uint16_t(snapshot), r = uint16_t(snapshot >> 16);
        check((l == 0 || l == 65535) && (r == 0 || r == 65535), "concurrent snapshot is coherent");
    }
    a.join(); b.join();
    check(rumble() == motors(65535, 65535), "concurrent sends retain both actuator states");

    std::atomic<bool> run{true};
    std::thread sender([&] { while (run.load(std::memory_order_relaxed)) send(left, 1, 1); });
    for (int i = 0; i < 1000; ++i) {
        invoke(set_styles, 6);
        check(rumble() == 0, "concurrent sends cannot bypass style filtering");
        invoke(set_styles, 7);
    }
    invoke(set_styles, 6);
    run.store(false, std::memory_order_relaxed);
    sender.join();
    check(rumble() == 0, "last filter transition wins against concurrent sends");
    invoke(set_styles, 7);
    send(right, 0.5f, 0);
    check(rumble() == motors(0, 32768), "concurrent filter clears both actuator states");
    send(right, 0, 0);
}

void test_vibration_batch(uint32_t& id) {
    uint32_t handles[] = {3, 0x103, 0x10003, 3, 0x20003};
    VibrationValue values[] = {
        {0.25f, 160, 0.75f, 320}, {1, 160, 1, 320},
        {0.25f, 160, 0.125f, 320}, {0.5f, 160, 0.125f, 320}, {1, 160, 1, 320},
    };
    const auto handles_ptr = reinterpret_cast<uint64_t>(handles);
    const auto values_ptr = reinterpret_cast<uint64_t>(values);
    uint32_t saved_handles[5];
    VibrationValue saved_values[5];
    std::memcpy(saved_handles, handles, sizeof(handles));
    std::memcpy(saved_values, values, sizeof(values));
    check(invoke(send_vibration_values, handles_ptr, values_ptr, 5) == 0, "batch returns success");
    check(rumble() == motors(32768, 16384), "batch skips invalid handles and last duplicate wins");
    check(std::memcmp(saved_handles, handles, sizeof(handles)) == 0 &&
          std::memcmp(saved_values, values, sizeof(values)) == 0, "batch never modifies input arrays");

    // w2 is signed and only its low 32 bits are passed to the SDK function.
    check(invoke(send_vibration_values, handles_ptr, values_ptr, 0xabcdef0100000001ull) == 0, "batch uses w2 count");
    check(rumble() == motors(49151, 16384), "single batch entry preserves other actuator");
    const uint32_t before = rumble();
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    for (uint64_t count : {0ull, 0xffffffffull, 0x80000000ull}) {
        check(invoke(send_vibration_values, max, max, count) == 0, "nonpositive batch is empty");
        check(rumble() == before, "empty batch leaves both actuators unchanged");
    }
    check(invoke(send_vibration_values, 0, values_ptr, 1) == 0, "null handles are ignored");
    check(invoke(send_vibration_values, handles_ptr, 0, 1) == 0, "null values are ignored");
    check(invoke(send_vibration_values, max - 3, values_ptr, 2) == 0, "overflowing handles are ignored");
    check(invoke(send_vibration_values, handles_ptr, max - 15, 2) == 0, "overflowing values are ignored");
    check(rumble() == before, "invalid batch arguments do not change motors");

    uint32_t pair[] = {3, 0x10003};
    VibrationValue pair_values[] = {
        {std::numeric_limits<float>::quiet_NaN(), 160, -1.0f, 320},
        {2.0f, 160, std::numeric_limits<float>::infinity(), 320},
    };
    const auto pair_ptr = reinterpret_cast<uint64_t>(pair);
    const auto pair_values_ptr = reinterpret_cast<uint64_t>(pair_values);
    check(invoke(send_vibration_values, pair_ptr, pair_values_ptr, 2) == 0, "two-actuator batch succeeds");
    check(rumble() == motors(0, 65535), "batch sanitizes and clamps each actuator independently");
    invoke(set_styles, 6);
    check(rumble() == 0, "style filter stops batch vibration");
    invoke(send_vibration_values, pair_ptr, pair_values_ptr, 2);
    check(rumble() == 0, "batch cannot vibrate a filtered style");
    invoke(set_styles, 7);
    uint32_t other = 1;
    invoke(set_ids, reinterpret_cast<uint64_t>(&other), 1);
    invoke(send_vibration_values, pair_ptr, pair_values_ptr, 2);
    check(rumble() == 0, "batch cannot vibrate a filtered player");
    invoke(set_ids, reinterpret_cast<uint64_t>(&id), 1);

    // A physical disconnect/reopen increments the same atomic generation used
    // by HLE publication and clears both strengths and both cached actuators.
    send(pair[0], 1, 0);
    send(pair[1], 1, 0);
    const uint32_t old_generation = nx::plat::pad_rumble_generation();
    nx::plat::reset_pad_rumble();
    check(nx::plat::pad_rumble_generation() == old_generation + 1u && rumble() == 0,
          "device reset advances generation and clears the motors");
    check(!nx::plat::try_publish_pad_rumble(old_generation, 65535, 65535) && rumble() == 0,
          "stale publication after reset is rejected");
    send(pair[1], 0.25f, 0);
    check(rumble() == motors(0, 16384), "single send after reconnect does not revive the other actuator");
    nx::plat::reset_pad_rumble();
    invoke(send_vibration_values, handles_ptr, values_ptr, 1);
    check(rumble() == motors(49151, 0), "batch after reopen does not revive the other actuator");

    // Two entries are published once as a pair, not as intermediate left/right
    // states, while another thread reads the shared host snapshot.
    pair_values[0] = {1, 0, 0, 0};
    pair_values[1] = {0, 0, 0, 0};
    invoke(send_vibration_values, pair_ptr, pair_values_ptr, 2);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) {
            pair_values[0].low = (i & 1) ? 1.0f : 0.0f;
            pair_values[1].low = (i & 1) ? 0.0f : 1.0f;
            invoke(send_vibration_values, pair_ptr, pair_values_ptr, 2);
        }
        done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) {
        const auto snapshot = rumble();
        check(snapshot == motors(65535, 0) || snapshot == motors(0, 65535), "batch publishes one atomic snapshot");
    }
    writer.join();

    // Race resets against real HLE sends, then assert the next generation starts
    // clean and a new one-sided batch cannot resurrect an old opposite side.
    std::thread sending([&] { for (int i = 0; i < 10000; ++i) send(pair[0], 1, 0); });
    for (int i = 0; i < 10000; ++i) nx::plat::reset_pad_rumble();
    sending.join();
    nx::plat::reset_pad_rumble();
    pair_values[1] = {0.5f, 0, 0, 0};
    invoke(send_vibration_values, pair_ptr + 4, pair_values_ptr + 16, 1);
    check(rumble() == motors(0, 32768), "racing resets leave no cached actuator after reopening");
    pair_values[1] = {0, 0, 0, 0};
    invoke(send_vibration_values, pair_ptr + 4, pair_values_ptr + 16, 1);
}
}
namespace nx {
std::atomic<uint64_t> g_frame{0};
HleRegistrar::HleRegistrar(const char* name, HleFn fn) { check(registry().emplace(name, fn).second, "unique registrations"); }
void log(const char*, ...) {}
uint64_t find_symbol(const char*) { std::abort(); }
void call(CPU&, uint64_t) { std::abort(); } // any SDK/applet fallback fails the test
namespace plat {
void read_pad(PadState& p) {
    ++host_reads;
    p.buttons |= host_pad.buttons;
    p.lx = host_pad.lx; p.ly = host_pad.ly; p.rx = host_pad.rx; p.ry = host_pad.ry;
}
}
}
int main() {
    unsetenv("NX_INPUT");
    uint32_t id = 0;
    auto idref = reinterpret_cast<uint64_t>(&id);
    invoke("14InitializeNpadEv");
    invoke("21InitializeTouchScreenEv");
    invoke("15InitializeMouseEv");
    invoke("18InitializeKeyboardEv");
    for (const auto& suffix : {
        "19GetTouchScreenStateILm1EEEvPNS0_16TouchScreenStateIXT_EEE",
        "19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE",
        "13GetMouseStateEPNS0_10MouseStateE", "16GetKeyboardStateEPNS0_13KeyboardStateE"}) {
        std::string name(suffix);
        size_t size = name.find("ILm16") != std::string::npos ? 0x290 : name.find("ILm1") != std::string::npos ? 0x38 : name.find("Mouse") != std::string::npos ? 0x28 : 0x30;
        Output out(size);
        invoke(name, out.ptr()); out.empty();
        auto first = nx::ld<uint64_t>(out.ptr());
        invoke(name, out.ptr()); out.empty();
        check(nx::ld<uint64_t>(out.ptr()) > first, "sampling advances without pad reads");
    }
    host_pad = {nx::plat::kA, -20000, 21000, 22000, -23000};
    check(invoke("15GetNpadStyleSetERKj", idref) == 1, "one FullKey style");
    Output pad(0x28);
    invoke("12GetNpadStateEPNS0_16NpadFullKeyStateERKj", pad.ptr(), idref);
    pad.guards();
    check(nx::ld<uint32_t>(pad.ptr() + 32) == 1, "FullKey connected");
    check(nx::ld<uint64_t>(pad.ptr() + 8) == (nx::plat::kA | nx::plat::kStickLLeft | nx::plat::kStickLUp | nx::plat::kStickRRight | nx::plat::kStickRDown), "host buttons and stick directions");
    check(nx::ld<int32_t>(pad.ptr() + 16) == -20000 && nx::ld<int32_t>(pad.ptr() + 28) == -23000, "stick offsets");
    check(nx::ld<uint32_t>(pad.ptr() + 36) == 0, "npad trailing bytes initialized");
    for (auto style : {"16NpadJoyDualState", "16NpadJoyLeftState", "17NpadJoyRightState", "17NpadHandheldState"}) {
        Output out(0x28); int before = host_reads;
        invoke(std::string("12GetNpadStateEPNS0_") + style + "ERKj", out.ptr(), idref); out.empty();
        check(host_reads == before, "unsupported style does not poll host");
        Output history(0x50);
        auto count = invoke(std::string("13GetNpadStatesEPNS0_") + style + "EiRKj", history.ptr(), 2, idref);
        check(count == 1, "unsupported style has one disconnected sample");
        check(nx::ld<uint32_t>(history.ptr() + 32) == 0, "plural unsupported style disconnected");
        check(nx::ld<uint64_t>(history.ptr() + 0x28) == 0xa5a5a5a5a5a5a5a5ull, "plural writes only returned count");
        history.guards();
    }
    for (uint32_t other : {1u, 7u, 0x10u, 0x20u}) {
        id = other;
        check(invoke("15GetNpadStyleSetERKj", idref) == 0, "other ID disconnected");
        Output out(0x28);
        invoke("12GetNpadStateEPNS0_16NpadFullKeyStateERKj", out.ptr(), idref); out.empty();
    }
    id = 0;
    test_vibration(id);
    test_vibration_batch(id);
    invoke("24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", 6);
    check(invoke("24GetSupportedNpadStyleSetEv") == 6, "style setter/getter round trip");
    check(invoke("15GetNpadStyleSetERKj", idref) == 0, "FullKey filtered by supported style");
    Output filtered(0x28);
    invoke("12GetNpadStateEPNS0_16NpadFullKeyStateERKj", filtered.ptr(), idref); filtered.empty();
    invoke("24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", 7);
    uint32_t supported[] = {1, 0x20};
    invoke("22SetSupportedNpadIdTypeEPKjm", reinterpret_cast<uint64_t>(supported), 2);
    check(invoke("15GetNpadStyleSetERKj", idref) == 0, "ID filtering");
    invoke("22SetSupportedNpadIdTypeEPKjm", idref, 1);
    check(invoke("15GetNpadStyleSetERKj", idref) == 1, "ID restored");
    for (uint32_t value : {0u, 1u, 2u}) {
        invoke("29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", value);
        check(invoke("29GetNpadHandheldActivationModeEv") == value, "activation round trip");
        check(invoke("15GetNpadStyleSetERKj", idref) == 1, "activation does not invent handheld connection");
    }
    for (uint32_t value : {1u, 0u}) {
        invoke("18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", value);
        check(invoke("18GetNpadJoyHoldTypeEv") == value, "hold round trip");
    }
    for (int i = 0; i < 20; ++i) {
        Output history(16 * 0x28);
        auto n = invoke("13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", history.ptr(), 16, idref);
        check(n == uint64_t(std::min(i + 1, 16)), "bounded FullKey history count"); history.guards();
        for (size_t k = 1; k < n; ++k) check(nx::ld<uint64_t>(history.ptr() + k * 0x28) < nx::ld<uint64_t>(history.ptr() + (k - 1) * 0x28), "newest-first history");
    }
    uint32_t handle = 0;
    Output handles(8);
    check(invoke("23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", handles.ptr(), 2, idref, 1) == 0, "no motion devices");
    handles.untouched();
    Output motion(0x60);
    invoke("21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", motion.ptr(), reinterpret_cast<uint64_t>(&handle)); motion.empty(8);
    check(invoke("22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", motion.ptr(), 1, reinterpret_cast<uint64_t>(&handle)) == 1, "one disconnected motion sample"); motion.empty(8);
    for (const auto& suffix : {"13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", "14GetMouseStatesEPNS0_10MouseStateEi", "22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE"}) {
        for (uint64_t count : {0ull, 0xffffffffull}) {
            Output out(0x60);
            check(invoke(suffix, out.ptr(), count, idref) == 0, "nonpositive capacity returns zero"); out.untouched();
        }
    }
    std::array<uint8_t, 0x430> arg{}; arg[0] = 1; arg[1] = 1;
    auto argptr = reinterpret_cast<uint64_t>(arg.data());
    auto saved = arg;
    check(invoke("21ShowControllerSupportERKNS0_20ControllerSupportArgE", argptr) == 0, "no-result support accepts available host controller");
    check(arg == saved, "const support arg is never used as output");
    Output result(8);
    check(invoke("21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", result.ptr(), argptr) == 0, "result overload succeeds"); result.guards();
    check(nx::ld<uint64_t>(result.ptr()) == 1, "entire result initialized with player count and No1");
    arg[0] = 2; arg[1] = 2;
    check(invoke("21ShowControllerSupportERKNS0_20ControllerSupportArgE", argptr) != 0, "two players cannot be fabricated");
    arg[0] = 1; arg[1] = 1;
    invoke("22SetSupportedNpadIdTypeEPKjm", 0, 0);
    check(invoke("21ShowControllerSupportERKNS0_20ControllerSupportArgE", argptr) != 0, "support respects ID filter");
    invoke("22SetSupportedNpadIdTypeEPKjm", idref, 1);
    uint32_t left_vibration_handle = 3;
    uint32_t right_vibration_handle = 0x10003;
    send(left_vibration_handle, 1, 0);
    send(right_vibration_handle, 0, 1);
    check(rumble() == motors(65535, 65535), "both actuators active before disconnect");
    invoke("14DisconnectNpadERKj", idref);
    check(rumble() == 0, "disconnect stops both actuators");
    send(left_vibration_handle, 1, 1);
    check(rumble() == 0, "disconnected device cannot resume rumble");
    Output disconnected_handles(8);
    check(invoke(get_vibration_handles, disconnected_handles.ptr(), 2, idref, 1) == 0, "disconnected device exposes no handles");
    disconnected_handles.untouched();
    check(invoke("15GetNpadStyleSetERKj", idref) == 0, "disconnect reflected by style query");
    check(invoke("21ShowControllerSupportERKNS0_20ControllerSupportArgE", argptr) != 0, "support does not falsely reconnect");
    std::puts("bayonetta3_hid_test: all API, ABI, sentinel and configuration checks passed");
}
