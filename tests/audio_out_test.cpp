// Standalone B3 AudioOut regression. Links the actual HLE source with synthetic kernel/device boundaries.
// Build: c++ -std=c++20 -pthread -DNX_BAYONETTA3 -Iruntime/include tests/audio_out_test.cpp -o audio_out_test
#include <cassert>
#include <cstdarg>
#include <stdexcept>
#include <functional>
#include <array>
#include <map>
#include <string>
#include "../runtime/src/hle_audio.cpp"

namespace {
std::map<std::string, nx::HleFn>& registry() { static std::map<std::string, nx::HleFn> r; return r; }
std::map<uint32_t, std::shared_ptr<nx::KObject>> handles;
uint32_t next_handle = 1;
std::mutex device_lock;
struct Pending {
    const int16_t* data; size_t bytes; void* token;
    std::shared_ptr<nx::plat::AudioPlayback> playback;
};
struct Accepted { std::shared_ptr<nx::plat::AudioPlayback> playback; size_t frames; };
std::deque<Pending> pending;
std::deque<Accepted> accepted;  // bytes were released, but the device has not consumed them
nx::plat::AudioDone done;
bool device_mode = false, reject_submit = false, synchronous_submit = false;
unsigned attachments = 0;
std::atomic<unsigned> notifications{0};
constexpr const char* prefix = "_ZN2nn5audio";
uint64_t invoke(const char* suffix, uint64_t a = 0, uint64_t b = 0, uint64_t d = 0, uint64_t e = 0) {
    auto it = registry().find(std::string(prefix) + suffix);
    assert(it != registry().end());
    nx::CPU c{}; c.x[0] = a; c.x[1] = b; c.x[2] = d; c.x[3] = e;
    it->second(c); return c.x[0];
}
template<class T> uint64_t addr(T& v) { return reinterpret_cast<uint64_t>(&v); }
void wait_for(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void complete_one(int16_t expected = 0x1234) {
    Pending p;
    { std::lock_guard lk(device_lock);
      assert(!pending.empty()); p = pending.front(); pending.pop_front();
      if (p.playback) accepted.push_back({p.playback, p.bytes / 4}); }
    assert(p.bytes == 3840);
    assert(p.data[0] == expected);
    done(p.token);
}
void consume_frames(size_t frames) {
    std::lock_guard lk(device_lock);
    while (frames) {
        assert(!accepted.empty());
        auto& a = accepted.front();
        const size_t take = std::min(frames, a.frames);
        a.playback->frames.fetch_add(take, std::memory_order_relaxed);
        frames -= take; a.frames -= take;
        if (!a.frames) accepted.pop_front();
    }
}
size_t accepted_count() { std::lock_guard lk(device_lock); return accepted.size(); }
bool fails(const std::function<void()>& f) {
    try { f(); } catch (const std::runtime_error&) { return true; }
    return false;
}
}
namespace nx {
HleRegistrar::HleRegistrar(const char* name, HleFn fn) { assert(registry().emplace(name, fn).second); }
void log(const char* format, ...) { va_list ap; va_start(ap, format); vprintf(format, ap); va_end(ap); puts(""); }
[[noreturn]] void fatal(const char* format, ...) {
    char text[1024]; va_list ap; va_start(ap, format); vsnprintf(text, sizeof text, format, ap); va_end(ap);
    throw std::runtime_error(text);
}
const char* hle_current_name() { return "audio regression"; }
std::string describe(uint64_t) { return "synthetic caller"; }
void notify_all() { ++notifications; }
uint32_t handle_add(std::shared_ptr<KObject> object) { handles[next_handle] = object; return next_handle++; }
void handle_close(uint32_t h) { handles.erase(h); }
uint64_t find_symbol(const char* name) {
    assert(std::string(name) == "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    return 1;
}
void call(CPU& c, uint64_t target) {
    assert(target == 1 && c.x[2] == 1 && c.x[3] == 1);
    assert(handles.count(c.x[1]));
    // Synthetic boundary models the confirmed SDK readable managed AutoClear event attachment.
    st<uint32_t>(c.x[0] + 0x14, uint32_t(c.x[1]));
    st<uint32_t>(c.x[0] + 0x18, 0);
    st<uint8_t>(c.x[0] + 0x28, 2);
    ++attachments;
}
namespace plat {
bool audio_open(AudioDone callback) { done = callback; return device_mode; }
bool audio_submit(const void* data, size_t bytes, void* ctx) {
    std::lock_guard lk(device_lock);
    if (reject_submit) return false;
    pending.push_back({static_cast<const int16_t*>(data), bytes, ctx, {}}); return true;
}
bool audio_submit_tracked(const void* data, size_t bytes, void* ctx, std::shared_ptr<AudioPlayback> playback) {
    {
        std::lock_guard lk(device_lock);
        if (reject_submit) return false;
        pending.push_back({static_cast<const int16_t*>(data), bytes, ctx, std::move(playback)});
    }
    // Completion is permitted before submit returns and destroys the submission token.
    if (synchronous_submit) complete_one(static_cast<const int16_t*>(data)[0]);
    return true;
}
uint32_t audio_queued() { std::lock_guard lk(device_lock); return uint32_t(pending.size()); }
}
}
int main(int argc, char** argv) {
    device_mode = argc > 1 && std::string(argv[1]) != "timer";
    reject_submit = argc > 1 && std::string(argv[1]) == "reject";
    synchronous_submit = argc > 1 && std::string(argv[1]) == "sync";
    const bool callbacks = device_mode && !reject_submit;
    std::array<uint8_t, 0x170> output{}, second{}, third{}, event{};
    uint64_t parameter = ~0ull;
    auto a = addr(output), b = addr(second), p = addr(parameter);
    invoke("27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", p);
    assert(parameter == 0xcafe000000000000ull);
    const char* open = "19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE";
    const char* start = "13StartAudioOutEPNS0_8AudioOutE";
    const char* stop = "12StopAudioOutEPNS0_8AudioOutE";
    const char* close = "13CloseAudioOutEPNS0_8AudioOutE";
    const char* state = "16GetAudioOutStateEPKNS0_8AudioOutE";
    const char* append = "20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE";
    const char* released = "25GetReleasedAudioOutBufferEPNS0_8AudioOutE";
    const char* played = "28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE";
    const char* set_buffer = "21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm";
    assert(invoke(open, a, p) == 0);
    assert(attachments == 0 && handles.empty()); // no-event overload must not reinterpret parameter as SystemEvent
    assert(invoke(open, a, p) == 0x1299);
    assert(invoke(open, b, p) == 0);
    assert(invoke(open, addr(third), p) == 0xa99);
    assert(invoke(state, a) == 1);
    assert(nx::ld<uint32_t>(a) == 48000 && nx::ld<uint32_t>(a + 4) == 2);
    assert(invoke("21GetAudioOutSampleRateEPKNS0_8AudioOutE", a) == 48000);
    assert(invoke("23GetAudioOutChannelCountEPKNS0_8AudioOutE", a) == 2);
    assert(invoke("23GetAudioOutSampleFormatEPKNS0_8AudioOutE", a) == 2);
    assert(invoke(played, a) == 0);
    assert(invoke("19RegisterBufferEventEPNS0_8AudioOutEPNS_2os11SystemEventE", a, addr(event)) == 0);
    assert(attachments == 1 && handles.size() == 1);
    const auto handle = nx::ld<uint32_t>(addr(event) + 0x14);
    auto signal = std::dynamic_pointer_cast<nx::KEvent>(handles.at(handle))->state;
    assert(!signal->signaled && nx::ld<uint32_t>(addr(event) + 0x18) == 0);
    assert(fails([&] { invoke("19RegisterBufferEventEPNS0_8AudioOutEPNS_2os11SystemEventE", a, addr(event)); }));
    assert(handles.size() == 1);

    std::array<uint64_t, 5> buf{}, buf2{};
    std::array<int16_t, 1920> pcm{}; pcm.fill(0x1234); // 960 stereo frames = 20 ms
    buf[0] = 0x11223344;
    invoke(set_buffer, addr(buf), addr(pcm), sizeof(pcm), sizeof(pcm));
    invoke(set_buffer, addr(buf2), addr(pcm), sizeof(pcm), sizeof(pcm));
    assert(buf[0] == 0x11223344 && buf[4] == 0);
    assert(invoke("28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", addr(buf)) == addr(pcm));
    assert(invoke("25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", addr(buf)) == sizeof(pcm));
    assert(invoke("27GetAudioOutBufferBufferSizeEPKNS0_14AudioOutBufferE", addr(buf)) == sizeof(pcm));
    assert(invoke(append, a, addr(buf)) == 1);
    assert(invoke(append, a, addr(buf)) == 0);
    assert(invoke(append, a, addr(buf2)) == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(invoke(released, a) == 0 && invoke(played, a) == 0);
    assert(invoke(start, a) == 0 && invoke(state, a) == 0);
    if (callbacks) {
        if (synchronous_submit) wait_for([] { return accepted_count() == 2; });
        else wait_for([] { return nx::plat::audio_queued() == 2; });
        assert(invoke(played, a) == 0);
        pcm.fill(0x5678); // submissions must own their bytes
        if (!synchronous_submit) complete_one();
        assert(signal->signaled && invoke(played, a) == 0); // release is not playback
        consume_frames(480);
        assert(invoke(played, a) == 480 && invoke(played, b) == 0);
        consume_frames(480);
    }
    wait_for([&] { return invoke(played, a) >= 960; });
    assert(signal->signaled);
    assert(invoke(released, a) == addr(buf));
    assert(invoke(released, b) == 0 && invoke(played, b) == 0);
    if (callbacks) {
        if (!synchronous_submit) complete_one();
        assert(invoke(played, a) == 960);
        consume_frames(960);
    }
    wait_for([&] { return invoke(played, a) == 1920; });
    assert(invoke(released, a) == addr(buf2));
    assert(invoke(released, a) == 0);
    // The timer may catch up its absolute deadline after the initial idle interval,
    // so the first pair alone has no minimum spacing. Sustained pacing is checked below.
    if (callbacks) {
        std::array<uint64_t, 5> other_buf{};
        invoke(set_buffer, addr(other_buf), addr(pcm), sizeof(pcm), sizeof(pcm));
        assert(invoke(append, b, addr(other_buf)) == 1);
        invoke(start, b);
        if (synchronous_submit) wait_for([] { return accepted_count() == 1; });
        else { wait_for([] { return nx::plat::audio_queued() == 1; }); complete_one(0x5678); }
        assert(invoke(released, b) == addr(other_buf));
        assert(invoke(played, b) == 0 && invoke(played, a) == 1920);
        consume_frames(240);
        assert(invoke(played, b) == 240 && invoke(played, a) == 1920);
        consume_frames(720);
        invoke(stop, b);
    }
    // Release polling does not consume AutoClear notification; a guest event wait owns that operation.
    assert(signal->signaled);
    signal->signaled = false; // synthetic guest wait, all completions above are finished
    invoke(stop, a);
    assert(invoke(state, a) == 1);
    assert(invoke(append, a, addr(buf)) == 1); // released buffer is reusable
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(invoke(released, a) == 0 && invoke(played, a) == 1920);
    invoke(start, a);
    if (callbacks) {
        if (synchronous_submit) wait_for([] { return accepted_count() == 1; });
        else wait_for([] { return nx::plat::audio_queued() == 1; });
    }
    invoke(close, a);
    assert(invoke(state, a) == 1);
    invoke(close, a); // idempotent
    assert(invoke(open, a, p) == 0); // same guest address, independent host generation
    if (callbacks) {
        if (!synchronous_submit) complete_one(0x5678);
        consume_frames(960); // retained old cursor cannot advance the reopened output
    }
    assert(invoke(played, a) == 0 && invoke(released, a) == 0);
    assert(!signal->signaled); // old completion cannot signal new output or old closed event
    nx::handle_close(handle); // guest SystemEvent owns the transferred handle
    assert(handles.empty());
    buf[3] = sizeof(pcm) + 4;
    assert(fails([&] { invoke(append, a, addr(buf)); }));
    buf[3] = sizeof(pcm); buf[4] = 4;
    assert(fails([&] { invoke(append, a, addr(buf)); }));
    invoke(close, a); invoke(close, b);
    nx::st<uint16_t>(p + 4, 6);
    assert(invoke(open, a, p) == 0); // existing surround input is folded to the stereo device
    assert(nx::ld<uint32_t>(a + 4) == 6); // accepted input format stored in the output object
    invoke(close, a);
    nx::st<uint16_t>(p + 4, 7);
    assert(fails([&] { invoke(open, a, p); }));
    nx::st<uint16_t>(p + 4, 2);
    event.fill(0);
    assert(invoke("19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", a, addr(event), p) == 0);
    assert(attachments == 2);
    nx::handle_close(nx::ld<uint32_t>(addr(event) + 0x14));
    invoke(close, a);
    assert(fails([&] { invoke(start, a); }));
    assert(fails([&] { invoke("32CreateAudioRendererManagerByHipcEv"); }));
    if (!callbacks) {
        // Eight 20-ms buffers must be paced, even allowing the initial deadline
        // catch-up and release-before-sleep behavior. This rejects a busy drain
        // without assuming where a fresh worker's first deadline falls.
        assert(invoke(open, a, p) == 0);
        std::array<std::array<uint64_t, 5>, 8> paced{};
        for (auto& buffer : paced) {
            invoke(set_buffer, addr(buffer), addr(pcm), sizeof(pcm), sizeof(pcm));
            assert(invoke(append, a, addr(buffer)) == 1);
        }
        const auto began = std::chrono::steady_clock::now();
        invoke(start, a);
        wait_for([&] { return invoke(played, a) == paced.size() * 960; });
        assert(std::chrono::steady_clock::now() - began >= std::chrono::milliseconds(80));
        for (auto& buffer : paced) assert(invoke(released, a) == addr(buffer));
        invoke(close, a);
    }
    assert(handles.empty());
    printf("PASS AudioOut %s: ABI, lifecycle, FIFO, pacing, separate playback/release, output isolation, close/reopen\n",
           argc > 1 ? argv[1] : "timer");
}
