// B3 hwopus IPC using host libopus. Original SDK wire ABI: docs/BAYONETTA3_OPUS.md.
// Keep the SDK's real decoder proxy/virtual methods; only replace the service endpoint.
#include "ipc.h"

#include <chrono>
#include <climits>
#include <cstddef>
#include <mutex>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace nx {
namespace {
struct OpusDecoder; // opaque stable libopus C ABI; no guest codec structs cross this boundary
struct OpusApi {
    int (*size)(int);
    int (*init)(OpusDecoder*, int32_t, int);
    int (*decode)(OpusDecoder*, const unsigned char*, int32_t, int16_t*, int, int);
    int (*samples)(const unsigned char*, int32_t, int32_t);
    int (*ctl)(OpusDecoder*, int, ...);
    const char* (*version)();
    OpusApi() {
#if defined(_WIN32)
        // Windows has no system libopus, so the build puts libopus.dll next to the executable (tools/build_opus_win.sh).
        // LoadLibraryA of a bare module name searches the executable's directory first, which is where it lives; the
        // explicit directory of the running executable is tried as well so a relocated data root still works.
        void* library = reinterpret_cast<void*>(LoadLibraryA("libopus.dll"));
        if (!library) {
            char exe[MAX_PATH];
            DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
            if (n && n < MAX_PATH) {
                std::string beside(exe, n);
                if (const size_t slash = beside.find_last_of("\\/"); slash != std::string::npos) {
                    beside.resize(slash + 1);
                    beside += "libopus.dll";
                    library = reinterpret_cast<void*>(LoadLibraryA(beside.c_str()));
                }
            }
        }
        if (!library) fatal("hwopus: libopus.dll not found next to the executable (build it with tools/build_opus_win.sh)");
        auto symbol = [&](const char* name) {
            void* address = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
            if (!address) fatal("hwopus: missing libopus export %s", name);
            return address;
        };
#elif defined(__linux__) || defined(__APPLE__)
        // The host's own libopus serves the decoder: the game's hwopus service owns the stream state, this only calls
        // the codec. The soname differs per platform, and a package manager may only have installed the versioned
        // name, so the candidates are tried in order.
#if defined(__linux__)
        const char* kCandidates[] = {"libopus.so.0", "libopus.so"};
#else
        const char* kCandidates[] = {"libopus.0.dylib", "libopus.dylib",
                                     "/opt/homebrew/lib/libopus.0.dylib", "/usr/local/lib/libopus.0.dylib"};
#endif
        void* library = nullptr;
        std::string tried;
        for (const char* name : kCandidates) {
            if (library) break;
            library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (!library) {
                if (!tried.empty()) tried += ", ";
                tried += name;
            }
        }
        if (!library) fatal("hwopus: host libopus unavailable (tried %s): %s", tried.c_str(), dlerror());
        auto symbol = [&](const char* name) {
            void* address = dlsym(library, name);
            if (!address) fatal("hwopus: missing libopus export %s", name);
            return address;
        };
#else
        fatal("hwopus: no libopus loading path for this host");
#endif
        size = reinterpret_cast<decltype(size)>(symbol("opus_decoder_get_size"));
        init = reinterpret_cast<decltype(init)>(symbol("opus_decoder_init"));
        decode = reinterpret_cast<decltype(decode)>(symbol("opus_decode"));
        samples = reinterpret_cast<decltype(samples)>(symbol("opus_packet_get_nb_samples"));
        ctl = reinterpret_cast<decltype(ctl)>(symbol("opus_decoder_ctl"));
        version = reinterpret_cast<decltype(version)>(symbol("opus_get_version_string"));
        // Process-lifetime library: services can be destroyed after other static objects.
        log("hwopus: host %s", version());
    }
};
OpusApi& opus() { static OpusApi api; return api; }
constexpr int kResetState = 4028, kGetFinalRange = 4031;
constexpr uint32_t kInvalidPacket = 0x226f; // SDK DecodeOpusInterleavedWithHardware maps this to codec error 4

struct Parameters {
    int32_t rate, channels;
    uint32_t large_frame, reserved;
};
static_assert(sizeof(Parameters) == 16);
Parameters parameters(const IpcCtx& ctx) {
    if (ctx.in_size < sizeof(Parameters)) fatal("hwopus cmd %u: truncated parameters", ctx.cmd);
    auto p = ctx.arg<Parameters>(0);
    if ((p.rate != 8000 && p.rate != 12000 && p.rate != 16000 && p.rate != 24000 && p.rate != 48000) ||
        (p.channels != 1 && p.channels != 2) || p.large_frame > 1 || p.reserved)
        fatal("hwopus cmd %u: unsupported rate=%d channels=%d large-frame=%u reserved=%u",
              ctx.cmd, p.rate, p.channels, p.large_frame, p.reserved);
    return p;
}
uint32_t work_size(const Parameters& p) {
    int bytes = opus().size(p.channels);
    if (bytes <= 0 || bytes > INT_MAX - 4095) fatal("hwopus: invalid host decoder size %d", bytes);
    // This is the actual host decoder storage requirement. SDK rounds up to a transfer-memory page.
    // Hardware DSP scratch/output staging is unnecessary for synchronous host decoding.
    return uint32_t(bytes);
}
uint32_t big32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

struct HardwareOpusDecoder : Service {
    Parameters parameter;
    std::shared_ptr<KTransferMemory> work;
    uint32_t state_bytes;
    std::mutex lock;
    OpusDecoder* state() const { return reinterpret_cast<OpusDecoder*>(work->addr); }
    HardwareOpusDecoder(Parameters p, std::shared_ptr<KTransferMemory> memory, uint32_t bytes)
        : Service("IHardwareOpusDecoder"), parameter(p), work(std::move(memory)), state_bytes(bytes) {
        const int result = opus().init(state(), p.rate, p.channels);
        if (result != 0) fatal("hwopus: opus_decoder_init failed (%d)", result);
    }
    // init-in-place state requires no opus_decoder_destroy (that API frees create-allocated memory).
    uint32_t handle(IpcCtx& ctx) override {
        if (ctx.cmd != 8) fatal("hwopus decoder cmd %u unsupported (only proven single-stream cmd8 enabled)", ctx.cmd);
        if (ctx.in_size < 1 || ctx.arg<uint8_t>(0) > 1) fatal("hwopus decode: invalid reset flag");
        const Buffer input = ctx.in_buf(0), output = ctx.out_buf(0);
        if (!input.addr || input.size < 8) return kInvalidPacket;
        const auto* packet = reinterpret_cast<const uint8_t*>(input.addr);
        const uint32_t length = big32(packet), expected_range = big32(packet + 4);
        if (!length || length > INT_MAX - 8 || uint64_t(length) + 8 > input.size) return kInvalidPacket;
        const int frames = opus().samples(packet + 8, int32_t(length), parameter.rate);
        if (frames <= 0) return kInvalidPacket;
        const int max_frames = parameter.rate * (parameter.large_frame ? 120 : 40) / 1000;
        if (frames > max_frames) fatal("hwopus: packet duration exceeds configured %d ms", parameter.large_frame ? 120 : 40);
        const size_t pcm_bytes = size_t(frames) * parameter.channels * sizeof(int16_t);
        if (!output.addr || output.size < pcm_bytes) fatal("hwopus: PCM output buffer too small (%llu < %zu)",
            (unsigned long long)output.size, pcm_bytes);
        // Validate first; decode transactionally so malformed/final-range-mismatched packets expose no PCM
        // and do not corrupt the stream's history. libopus explicitly permits copying its contiguous state.
        std::lock_guard lk(lock);
        std::vector<uint8_t> saved(state_bytes);
        std::memcpy(saved.data(), state(), state_bytes);
        std::vector<int16_t> pcm(size_t(frames) * parameter.channels);
        auto restore = [&] { std::memcpy(state(), saved.data(), state_bytes); };
        if (ctx.arg<uint8_t>(0)) {
            int result = opus().ctl(state(), kResetState);
            if (result) { restore(); fatal("hwopus: OPUS_RESET_STATE failed (%d)", result); }
        }
        const auto start = std::chrono::steady_clock::now();
        const int decoded = opus().decode(state(), packet + 8, int32_t(length), pcm.data(), frames, 0);
        const int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (decoded < 0) { restore(); return kInvalidPacket; }
        if (decoded != frames) { restore(); fatal("hwopus: packet sample count disagrees with decode"); }
        if (expected_range) {
            uint32_t actual_range = 0;
            int result = opus().ctl(state(), kGetFinalRange, &actual_range);
            if (result) { restore(); fatal("hwopus: OPUS_GET_FINAL_RANGE failed (%d)", result); }
            if (actual_range != expected_range) { restore(); return kInvalidPacket; }
        }
        std::memcpy(reinterpret_cast<void*>(output.addr), pcm.data(), pcm_bytes);
        ctx.push<int32_t>(int32_t(length + 8)); // compressed bytes consumed, including Nintendo packet header
        ctx.push<int32_t>(decoded);             // samples per channel
        ctx.push<int64_t>(elapsed_us);           // SDK multiplies this by 1000 for TimeSpan nanoseconds
        return 0;
    }
};

struct HardwareOpusManager : Service {
    HardwareOpusManager() : Service("hwopus") {}
    uint32_t handle(IpcCtx& ctx) override {
        if (ctx.cmd != 5 && ctx.cmd != 4)
            fatal("hwopus manager cmd %u unsupported (only proven single-stream Ex commands 4/5 enabled)", ctx.cmd);
        const auto p = parameters(ctx);
        const uint32_t required = work_size(p);
        if (ctx.cmd == 5) {
            ctx.push<uint32_t>(required);
            return 0;
        }
        if (ctx.in_size < 20 || ctx.in_copy.size() != 1 || !ctx.in_move.empty())
            fatal("hwopus open: expected 20-byte parameters and one copied transfer-memory handle");
        const uint32_t supplied = ctx.arg<uint32_t>(16);
        auto memory = std::dynamic_pointer_cast<KTransferMemory>(handle_get(ctx.in_copy[0]));
        if (!memory || !memory->addr || (memory->addr & 4095) || (supplied & 4095) ||
            supplied < required || supplied > memory->size)
            fatal("hwopus open: invalid/undersized transfer-memory work buffer (need %u, supplied %u)", required, supplied);
        ctx.push_object(std::make_shared<HardwareOpusDecoder>(p, std::move(memory), required));
        return 0;
    }
};
ServiceRegistrar register_hwopus("hwopus", [] { return std::make_shared<HardwareOpusManager>(); });
} // namespace
} // namespace nx
