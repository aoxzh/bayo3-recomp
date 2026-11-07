// Standalone production-include test; no game, Vulkan driver or CMake changes.
// From the repository root (use the installed clang++):
// clang++ -std=c++20 -march=x86-64-v3 -DNX_BAYONETTA3=1 -ffunction-sections -fdata-sections \
//   -Iruntime/include -Iruntime/src -Iprivate/toolchains/Vulkan-Headers-1.4.357/include \
//   tests/bayonetta3_nvn_dispatch_test.cpp -Wl,--gc-sections -pthread -o /tmp/b3-nvn-dispatch-test
// env -u NX_B3_DIAGNOSTIC -u NX_NVN_NO_DRIVER -u NX_NVN_REAL /tmp/b3-nvn-dispatch-test strict
// env -u NX_NVN_NO_DRIVER -u NX_NVN_REAL NX_B3_DIAGNOSTIC=1 /tmp/b3-nvn-dispatch-test diagnostic
// Also run NX_B3_DIAGNOSTIC=0 and =true with argument strict.
#include <nx/runtime.h>
#include "platform.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

// Only registration is disabled. proc_address, both query entry points,
// trace_stub and real_proc/real_proc_slow are included unchanged.
#undef NX_HLE
#define NX_HLE(sym, fn)
#include "../runtime/src/nvn/nvn_trace.cpp"

namespace fixture {
constexpr uint64_t loader = 0x1000, sdk_unimplemented = 0x2000, sdk_builder = 0x3000;
constexpr uint64_t implemented_address = 0x4000;
const char* unsupported = "nvnQueueInitialize";
const char* builder = "nvnTextureBuilderSetDefaults";
const char* implemented = "nvnDeviceInitialize";
const char* absent = "nvnB3DefinitelyAbsentProc";
struct Fatal : std::runtime_error { using std::runtime_error::runtime_error; };
std::map<std::string, std::pair<uint64_t, nx::HleFn>> thunks;
std::string current_name;
unsigned sdk_lookups = 0, sdk_executions = 0, hle_executions = 0;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void hle(nx::CPU& c) { ++hle_executions; c.x[0] = 0x1234; }
void invoke(uint64_t address, nx::CPU& c) {
    if (address == implemented_address) { hle(c); return; }
    for (const auto& [name, entry] : thunks) {
        if (entry.first == address) { current_name = name; entry.second(c); return; }
    }
    nx::call(c, address);
}
}

namespace nx {
bool g_trace = false;
// Host policy seam, matching loader.cpp's exact "1" opt-in. This fixture tests
// dispatch under both policies; it does not claim to test loader.cpp itself.
bool strict_runtime() { const char* p = std::getenv("NX_B3_DIAGNOSTIC"); return !p || std::strcmp(p, "1") != 0; }
void log(const char*, ...) {}
[[noreturn]] void fatal(const char* fmt, ...) {
    char message[1024]; va_list ap; va_start(ap, fmt);
    std::vsnprintf(message, sizeof(message), fmt, ap); va_end(ap);
    throw fixture::Fatal(message);
}
std::string describe(uint64_t) { return "fixture-lr"; }
const char* hle_current_name() { return fixture::current_name.c_str(); }
HleFn hle_lookup(const char* name) { return name == std::string(fixture::implemented) ? fixture::hle : nullptr; }
uint64_t hle_address(const char* name) {
    fixture::check(hle_lookup(name) != nullptr, "unimplemented name passed to hle_address");
    return fixture::implemented_address;
}
uint64_t hle_address_or(const char* name, HleFn fallback) {
    if (hle_lookup(name)) return hle_address(name);
    auto [it, inserted] = fixture::thunks.try_emplace(name, 0, fallback);
    if (inserted) it->second.first = 0x10000 + fixture::thunks.size() * 16;
    return it->second.first;
}
uint64_t find_symbol(const char* name) {
    fixture::check(std::strcmp(name, "nvnBootstrapLoader") == 0, "unexpected SDK symbol");
    return fixture::loader;
}
uint64_t kernel_alloc(uint64_t size, uint64_t) {
    static unsigned char scratch[0x1000];
    fixture::check(size <= sizeof(scratch), "unexpected scratch allocation");
    return reinterpret_cast<uint64_t>(scratch);
}
// Synthetic SDK export table, not target execution. Querying its loader is
// allowed; invoking an unsupported exported function is counted separately.
void call(CPU& c, uint64_t target) {
    if (target == fixture::loader) {
        ++fixture::sdk_lookups;
        const std::string name(reinterpret_cast<const char*>(c.x[0]));
        c.x[0] = name == fixture::unsupported ? fixture::sdk_unimplemented :
                 name == fixture::builder ? fixture::sdk_builder : 0;
        return;
    }
    ++fixture::sdk_executions;
    fixture::check(target == fixture::sdk_builder, "unsupported SDK function was executed");
    c.x[0] = 0x5678;
}
namespace plat {
std::string data_root() { return {}; }
FILE* fopen_utf8(const std::string&, const char*) { return nullptr; } // no trace file side effects
}
}

int main(int argc, char** argv) {
    using namespace fixture;
    try {
        check(argc == 2, "expected strict or diagnostic argument");
        const bool strict = std::string(argv[1]) == "strict";
        check(strict || std::string(argv[1]) == "diagnostic", "invalid test mode");
        check(nx::strict_runtime() == strict, "requested mode does not match NX_B3_DIAGNOSTIC");
        check(!std::getenv("NX_NVN_NO_DRIVER") && !std::getenv("NX_NVN_REAL"), "unset NX_NVN_NO_DRIVER and NX_NVN_REAL for deterministic tests");
        nx::CPU c{}; c.x[30] = 0xabcdef;
        auto query = [&](const char* name) {
            c.x[0] = 0x55; c.x[1] = reinterpret_cast<uint64_t>(name);
            nx::nvn_device_get_proc_address(c);
            return c.x[0];
        };
        auto address = query(implemented);
        check(address == implemented_address && sdk_lookups == 0 && hle_executions == 0, "implemented HLE query changed");
        invoke(address, c);
        check(hle_executions == 1 && c.x[0] == 0x1234, "implemented HLE invocation changed");
        address = query(builder);
        check(address == sdk_builder && sdk_executions == 0, "pure SDK driver proc not preserved");
        invoke(address, c);
        check(sdk_executions == 1 && c.x[0] == 0x5678, "pure driver invocation changed");

        const unsigned executed_before = sdk_executions;
        address = query(unsupported);
        check(address != 0 && address != sdk_unimplemented, "known unsupported SDK proc needs a guarded thunk");
        check(thunks.at(unsupported).second == nx::trace_stub, "wrong fallback handler");
        check(sdk_executions == executed_before && nx::g_calls.empty(), "query executed unsupported function");
        check(query(unsupported) == address, "repeated lookup changed thunk identity");
        c.x[0] = reinterpret_cast<uint64_t>(unsupported);
        nx::nvn_bootstrap_loader(c);
        check(c.x[0] == address, "bootstrap and device dispatch disagree");
        c.x[0] = 0xdead;
        bool failed = false;
        try { invoke(address, c); }
        catch (const Fatal& e) {
            failed = true;
            check(std::string(e.what()).find("unimplemented NVN call: nvnQueueInitialize") != std::string::npos,
                  "missing strict invocation diagnostic");
        }
        check(failed == strict, "strict/diagnostic invocation boundary changed");
        if (strict) check(c.x[0] == 0xdead && nx::g_calls.empty(), "strict invocation performed placeholder work");
        else check(c.x[0] == 1 && nx::g_calls.at(unsupported) == 1, "diagnostic Initialize placeholder changed");
        check(sdk_executions == executed_before, "unsupported call entered real SDK implementation");
        address = query(absent);
        if (strict) check(address == 0 && !thunks.count(absent), "SDK-absent name must return null without thunk");
        else check(address != 0 && thunks.at(absent).second == nx::trace_stub, "diagnostic lookup incorrectly used strict rejection");
        check(nx::proc_address(c, "") == 0, "empty query must return null");
        std::printf("bayonetta3_nvn_dispatch: %s PASS (HLE, driver, deferred strict failure, absent lookup)\n", argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bayonetta3_nvn_dispatch: FAIL: %s\n", e.what());
        return 1;
    }
}
