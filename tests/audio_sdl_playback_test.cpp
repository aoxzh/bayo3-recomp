// Exercise the production Linux SDL callback with a controlled SDL stream.
// No audio device or game is started.
#include <SDL3/SDL.h>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static SDL_AudioStream* SDLCALL mock_open(SDL_AudioDeviceID, const SDL_AudioSpec*, SDL_AudioStreamCallback, void*);
static bool SDLCALL mock_resume(SDL_AudioStream*);
static bool SDLCALL mock_pause(SDL_AudioStream*);
static int SDLCALL mock_queued(SDL_AudioStream*);
static int SDLCALL mock_available(SDL_AudioStream*);
static bool SDLCALL mock_put(SDL_AudioStream*, const void*, int);
static const char* SDLCALL mock_error();

#define SDL_OpenAudioDeviceStream mock_open
#define SDL_ResumeAudioStreamDevice mock_resume
#define SDL_PauseAudioStreamDevice mock_pause
#define SDL_GetAudioStreamQueued mock_queued
#define SDL_GetAudioStreamAvailable mock_available
#define SDL_PutAudioStreamData mock_put
#define SDL_GetError mock_error
#define main nvn_unused_platform_main
#include "../runtime/src/platform_linux.cpp"
#undef main
#undef SDL_OpenAudioDeviceStream
#undef SDL_ResumeAudioStreamDevice
#undef SDL_PauseAudioStreamDevice
#undef SDL_GetAudioStreamQueued
#undef SDL_GetAudioStreamAvailable
#undef SDL_PutAudioStreamData
#undef SDL_GetError

namespace {
using namespace nx::plat;
SDL_AudioStream* const stream = reinterpret_cast<SDL_AudioStream*>(uintptr_t(0x1234));
int queued = 0;
int override_queued = -2; // -2 means use the real test queue
bool fail_put = false;
int put_calls = 0, available_calls = 0, opens = 0, resumes = 0, pauses = 0, releases = 0;
std::vector<uint8_t> last_put;
std::vector<std::string> logs;

void check_unlocked() {
    std::atomic<bool> acquired{false};
    std::thread other([&] {
        if (g_audio.try_lock()) {
            acquired = true;
            g_audio.unlock();
        }
    });
    other.join();
    assert(acquired.load());
}
struct Buffer {
    std::vector<uint8_t> bytes;
    bool released = false;
};
void done(void* context) {
    // Also proves the release callback is outside g_audio: it may submit again.
    (void)audio_queued();
    ++releases;
    if (context) {
        auto& b = *static_cast<Buffer*>(context);
        assert(!b.released);
        b.released = true;
        std::fill(b.bytes.begin(), b.bytes.end(), 0xee);
    }
}
void callback(int additional) { audio_cb(nullptr, stream, additional, 123456); }
void consume(int n) { assert(n >= 0 && n <= queued); queued -= n; }
uint64_t frames(const std::shared_ptr<AudioPlayback>& p) { return p->frames.load(std::memory_order_relaxed); }
void submit(Buffer& b, const std::shared_ptr<AudioPlayback>& p) {
    assert(audio_submit_tracked(b.bytes.data(), b.bytes.size(), &b, p));
}
}

SDL_AudioStream* SDLCALL mock_open(SDL_AudioDeviceID, const SDL_AudioSpec* spec, SDL_AudioStreamCallback cb, void*) {
    assert(spec && spec->format == SDL_AUDIO_S16LE && spec->channels == 2 && spec->freq == 48000 && cb);
    ++opens;
    return stream;
}
bool SDLCALL mock_resume(SDL_AudioStream* s) { assert(s == stream); ++resumes; return true; }
bool SDLCALL mock_pause(SDL_AudioStream* s) { assert(s == stream); ++pauses; return true; }
int SDLCALL mock_queued(SDL_AudioStream* s) {
    assert(s == stream);
    check_unlocked();
    return override_queued == -2 ? queued : override_queued;
}
int SDLCALL mock_available(SDL_AudioStream*) { ++available_calls; return 20000; }
bool SDLCALL mock_put(SDL_AudioStream* s, const void* data, int n) {
    assert(s == stream && n > 0);
    check_unlocked();
    ++put_calls;
    const auto* p = static_cast<const uint8_t*>(data);
    last_put.assign(p, p + n);
    if (fail_put) { fail_put = false; return false; }
    queued += n;
    return true;
}
const char* SDLCALL mock_error() { return "mock SDL error"; }
namespace nx {
void log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    logs.emplace_back(buf);
}
}

int main() {
    uint8_t byte = 0x42;
    auto a = std::make_shared<AudioPlayback>();
    auto b = std::make_shared<AudioPlayback>();
    assert(!audio_submit_tracked(&byte, 1, nullptr, a));
    assert(audio_open(done));
    assert(opens == 1 && resumes == 1);
    assert(audio_open(done) && opens == 1); // no reset of the live stream
    audio_pause(true);
    audio_pause(false);
    assert(pauses == 1 && resumes == 2);

    Buffer first{{1,2,3,4,5,6,7,8}};
    Buffer other{{9,10,11,12}};
    Buffer second{{20,21,22,23,24,25,26,27,28,29,30,31}};
    submit(first, a);
    assert(audio_submit(other.bytes.data(), other.bytes.size(), &other));
    submit(second, b);
    callback(4);
    assert(!first.released && audio_queued() == 3 && queued == 4);
    consume(3);
    callback(4);
    assert(first.released && audio_queued() == 2 && queued == 5 && frames(a) == 0);
    assert((last_put == std::vector<uint8_t>{5,6,7,8})); // release already poisoned guest source
    consume(5);
    callback(0); // must account consumption even without a request for more input
    assert(frames(a) == 2 && frames(b) == 0 && queued == 0);
    callback(9); // 4 untracked bytes followed by 5 from B
    assert(other.released && !second.released && queued == 9);
    consume(6); // untracked 4 + first 2 of B
    callback(0);
    assert(frames(a) == 2 && frames(b) == 0);
    callback(8); // remaining B7 plus one byte underrun silence
    assert(second.released && queued == 11 && last_put.back() == 0);
    consume(10);
    callback(0);
    assert(frames(a) == 2 && frames(b) == 3);
    consume(1);
    callback(0);
    assert(frames(a) == 2 && frames(b) == 3 && available_calls == 0);

    // A request larger than the local 4096-byte buffer is journalled as
    // several puts. Partial consumption and underrun bytes still stay separate.
    auto wide = std::make_shared<AudioPlayback>();
    Buffer wide_data{std::vector<uint8_t>(5001, 0x44)};
    submit(wide_data, wide);
    callback(5004);
    assert(wide_data.released && queued == 5004);
    consume(4099);
    callback(0);
    assert(frames(wide) == 1024);
    consume(902);
    callback(0);
    assert(frames(wide) == 1250);
    consume(3);
    callback(0);
    assert(frames(wide) == 1250);

    // Zero-length submissions still release their context without reading a
    // null buffer or crediting the following underrun to that output.
    Buffer empty{};
    assert(audio_submit_tracked(nullptr, 0, &empty, wide));
    callback(4);
    assert(empty.released && queued == 4);
    consume(4);
    callback(0);
    assert(frames(wide) == 1250);

    // Release and output closure happen before SDL consumes the old cursor.
    auto old = std::make_shared<AudioPlayback>();
    auto old_inspect = old;
    std::weak_ptr<AudioPlayback> weak_old = old;
    Buffer old_data{{1,1,1,1}};
    submit(old_data, old);
    callback(4);
    assert(old_data.released);
    old.reset();
    auto replacement = std::make_shared<AudioPlayback>();
    Buffer new_data{{2,2,2,2}};
    submit(new_data, replacement);
    callback(4);
    consume(4);
    callback(0);
    assert(frames(old_inspect) == 1 && frames(replacement) == 0);
    old_inspect.reset();
    assert(weak_old.expired());
    consume(4);
    callback(0);
    assert(frames(replacement) == 1 && weak_old.expired());

    // A failed put still releases the input, but it cannot count as consumed.
    auto failed = std::make_shared<AudioPlayback>();
    Buffer bad{{7,7,7,7}};
    submit(bad, failed);
    fail_put = true;
    callback(4);
    assert(bad.released && queued == 0 && frames(failed) == 0);
    callback(0);
    assert(frames(failed) == 0);
    Buffer good{{8,8,8,8}};
    submit(good, failed);
    callback(4);
    consume(4);
    callback(0);
    assert(frames(failed) == 1);

    auto invalid = std::make_shared<AudioPlayback>();
    Buffer data{{1,2,3,4,5,6,7,8}};
    submit(data, invalid);
    callback(8);
    consume(4);
    override_queued = -1;
    callback(0);
    override_queued = 9; // impossible for the 8 known bytes
    callback(0);
    assert(frames(invalid) == 0);
    override_queued = -2;
    callback(0);
    assert(frames(invalid) == 1);
    consume(4);
    callback(0);
    assert(frames(invalid) == 2 && queued == 0);

    // Model a >INT_MAX input queue without allocating the payload. SDL's
    // INT_MAX result is clamped and must not advance this callback.
    auto large = std::make_shared<AudioPlayback>();
    const uint64_t max_queue = uint64_t(std::numeric_limits<int>::max());
    g_playback_timeline.append(large, max_queue + 8);
    override_queued = std::numeric_limits<int>::max();
    callback(0);
    assert(frames(large) == 0 && g_playback_timeline.queued_bytes() == max_queue + 8);
    override_queued = 8;
    callback(0);
    assert(frames(large) == max_queue / 4);
    override_queued = 0;
    callback(0);
    assert(frames(large) == (max_queue + 8) / 4);
    override_queued = -2;

    // A persistent query error with many distinct spans is bounded and
    // freezes only accounting. Real submissions and releases still work.
    auto x = std::make_shared<AudioPlayback>();
    auto y = std::make_shared<AudioPlayback>();
    override_queued = -1;
    const int release_before = releases;
    for (size_t i = 0; i < detail::AudioPlaybackTimeline::kMaxSpans; ++i)
        assert(audio_submit_tracked(&byte, 1, nullptr, i % 2 ? y : x));
    callback(int(detail::AudioPlaybackTimeline::kMaxSpans));
    assert(!g_playback_timeline.lost() && g_playback_timeline.span_count() == detail::AudioPlaybackTimeline::kMaxSpans);
    assert(audio_submit_tracked(&byte, 1, nullptr, x));
    callback(1);
    assert(g_playback_timeline.lost() && g_playback_timeline.span_count() == 0 && logs.size() == 1);
    assert(releases == release_before + int(detail::AudioPlaybackTimeline::kMaxSpans) + 1);
    assert(audio_submit_tracked(&byte, 1, nullptr, y));
    callback(1);
    assert(logs.size() == 1 && audio_queued() == 0);
    override_queued = -2;
    consume(queued);
    callback(0);
    assert(frames(x) == 0 && frames(y) == 0);

    // Simulate destruction of the prior stream before opening a new one.
    g_stream = nullptr;
    assert(audio_open(done));
    assert(opens == 2 && !g_playback_timeline.lost());
    Buffer after{{9,9,9,9}};
    submit(after, x);
    callback(4);
    consume(4);
    callback(0);
    assert(frames(x) == 1 && frames(y) == 0 && available_calls == 0);
    assert(put_calls > 0);
    std::puts("production SDL callback: tracking, failures, bounds, and stream reset passed");
}
