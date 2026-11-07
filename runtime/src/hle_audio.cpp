// nn::audio AudioOut at the API level. FMOD (inside main) mixes s16 stereo 48 kHz into AudioOutBuffers and appends
// them; a player thread hands each one to the platform's audio device (XAudio2 / AAudio / SDL3) and returns it
// when the backend no longer needs its bytes. Linux B3 playback counts separately track SDL consumption;
// buffer release remains available early enough for FMOD to refill the queue.
// Without a device the same pacing runs on a timer. Audio input reports no devices. SDK 16 games (Wonder) use the
// AudioRenderer instead, rendered in process; its final mix goes to the same device (see below).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "audio_playback.h"
#include "kernel.h"
#include "platform.h"

namespace nx {

namespace {

// NX_AUDIO_DUMP=file.wav: 10 s of the final stereo mix. Both audio paths use it — Wonder's AudioRenderer
// (render_submit, below) and Bayonetta 3's AudioOut (b3_player) — so it lives outside either #if block:
// it used to sit in the Wonder-only branch, which is why a B3 run with NX_AUDIO_DUMP produced no file at all.
void write_wav(const char* path, const std::vector<int16_t>& pcm) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = uint32_t(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(2), u32(48000), u32(48000 * 4), u16(4), u16(16);
    std::fwrite("data", 1, 4, f), u32(bytes), std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    log("audio: wrote %s (%zu frames)", path, pcm.size() / 2);
}

#if !defined(NX_BAYONETTA3)
constexpr uint64_t kBytesPerSecond = 48000 * 2 * 2;
constexpr uint32_t kMaxInFlight = 4;  // buffers queued in the device (4 x ~21 ms)

// Our AudioOutBuffer layout (only our HLE reads it): {void* data; u64 capacity; u64 size}.
struct Out {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<uint64_t> queued, released;
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    bool started = false;
};
Out g_out;

void release(void* ctx) {
    std::lock_guard lk(g_out.lock);
    g_out.released.push_back(reinterpret_cast<uint64_t>(ctx));
    g_out.event->signaled = true;
    notify_all();
    g_out.cv.notify_all();
}

// The platform device has one completion callback: AudioOut buffers are guest addresses, the renderer's blocks
// live in g_render (below) and go back to its free list.
void render_block_done(void* ctx);
bool render_owns(void* ctx);

void device_done(void* ctx) {
    if (render_owns(ctx))
        render_block_done(ctx);
    else
        release(ctx);
}

bool device_open() {
    static const bool ok = plat::audio_open(device_done);
    return ok;
}

void player() {
    bool device = device_open();
    log("audio: %s", device ? "device output (48 kHz stereo s16)" : "no audio device, timer pacing");
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;) {
        uint64_t buf = 0, size = 0;
        {
            std::unique_lock lk(g_out.lock);
            auto ready = [&] {
                return g_out.started && !g_out.queued.empty() && (!device || plat::audio_queued() < kMaxInFlight);
            };
            g_out.cv.wait_for(lk, std::chrono::milliseconds(5), ready);
            if (!ready()) continue;
            buf = g_out.queued.front();
            g_out.queued.pop_front();
            size = ld<uint64_t>(buf + 16);
        }
        if (device && plat::audio_submit(reinterpret_cast<const void*>(ld<uint64_t>(buf)), size,
                                         reinterpret_cast<void*>(buf)))
            continue;
        next = std::max(next, clock::now() - std::chrono::milliseconds(50));
        next += std::chrono::microseconds(size * 1000000 / kBytesPerSecond);
        std::this_thread::sleep_until(next);
        release(reinterpret_cast<void*>(buf));
    }
}

void InitializeAudioOutParameter(CPU& c) {  // {u32 sample_rate; u16 channels; u16 reserved}
    st<uint32_t>(c.x[0], 0);
    st<uint16_t>(c.x[0] + 4, 0);
    st<uint16_t>(c.x[0] + 6, 0);
}

void OpenDefaultAudioOut(CPU& c) {  // (AudioOut*, os::SystemEvent*, const AudioOutParameter&)
    uint64_t out = c.x[0], sysev = c.x[1];
    // Give the guest a system event backed by our kernel event: nn::os::AttachSystemEvent(type, read, managed,
    // write, managed, clear mode = manual).
    auto w = std::make_shared<KEvent>();
    auto r = std::make_shared<KEvent>();
    w->state = r->state = g_out.event;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = sysev;
    c.x[1] = hr;
    c.x[2] = 1;
    c.x[3] = hw;
    c.x[4] = 1;
    c.x[5] = 1;  // manual clear
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    st<uint64_t>(out, 0xA0D10);  // opaque
    static std::once_flag once;
    std::call_once(once, [] { std::thread(player).detach(); });
    c.x[0] = 0;
}

void StartAudioOut(CPU& c) {
    std::lock_guard lk(g_out.lock);
    g_out.started = true;
    g_out.cv.notify_all();
    c.x[0] = 0;
}

void StopAudioOut(CPU&) {
    std::lock_guard lk(g_out.lock);
    g_out.started = false;
}

void CloseAudioOut(CPU&) {}

void SetAudioOutBufferInfo(CPU& c) {  // (AudioOutBuffer*, void* data, size_t capacity, size_t size)
    st<uint64_t>(c.x[0], c.x[1]);
    st<uint64_t>(c.x[0] + 8, c.x[2]);
    st<uint64_t>(c.x[0] + 16, c.x[3]);
}

void GetAudioOutBufferDataPointer(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0]); }

void AppendAudioOutBuffer(CPU& c) {  // (AudioOut*, AudioOutBuffer*)
    std::lock_guard lk(g_out.lock);
    g_out.queued.push_back(c.x[1]);
    g_out.cv.notify_all();
    c.x[0] = 1;  // bool: appended
}

void GetReleasedAudioOutBuffer(CPU& c) {
    std::lock_guard lk(g_out.lock);
    if (g_out.released.empty()) {
        c.x[0] = 0;
        return;
    }
    c.x[0] = g_out.released.front();
    g_out.released.pop_front();
    if (g_out.released.empty()) g_out.event->signaled = false;
}

#else
// B3 1.2.0 SDK: AudioOut metadata +0 rate/+4 channels/+8 format/+c state;
// AudioOutBuffer +0 link/+8 data/+10 capacity/+18 size/+20 offset (0x28 bytes).
// See private/audio-validation for original NSO hashes, imports and disassembly.
struct B3Out {
    std::mutex lock;
    std::deque<uint64_t> queued, released;
    std::unordered_set<uint64_t> owned;
    std::shared_ptr<EventState> event = std::make_shared<EventState>();
    std::shared_ptr<plat::AudioPlayback> playback = std::make_shared<plat::AudioPlayback>();
    uint32_t channels = 2;  // what the game opened with (0: the SDK's default 2)
    bool started = false, closed = false;
};
struct B3Submission {
    std::shared_ptr<B3Out> out;
    uint64_t buffer;
    std::vector<int16_t> pcm;  // platform callbacks never dereference guest memory
    bool count_on_release = true;  // only backends without a separate playback cursor
};
struct B3Audio {
    std::mutex lock;
    std::condition_variable cv;
    std::unordered_map<uint64_t, std::shared_ptr<B3Out>> outputs;
    std::thread worker;
    bool shutdown = false;
    ~B3Audio() {
        { std::lock_guard lk(lock); shutdown = true; }
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
};
B3Audio g_b3audio;
std::atomic<uint64_t> g_b3_appends{0};  // b3_player's periodic stats read this
std::atomic<uint64_t> g_b3_playback_queries{0};

std::shared_ptr<B3Out> b3_out(uint64_t address) {
    std::lock_guard lk(g_b3audio.lock);
    auto it = g_b3audio.outputs.find(address);
    if (it == g_b3audio.outputs.end())
        fatal("B3 AudioOut: operation on unopened object %llx", (unsigned long long)address);
    return it->second;
}

void b3_done(void* ctx) {
    std::unique_ptr<B3Submission> submission(static_cast<B3Submission*>(ctx));
    auto& out = *submission->out;
    {
        std::lock_guard lk(out.lock);
        if (out.closed) return;
        if (submission->count_on_release)
            out.playback->frames.fetch_add(submission->pcm.size() / 2, std::memory_order_relaxed);
        out.released.push_back(submission->buffer);
        out.event->signaled = true;
    }
    notify_all();
    // This buffer left the device queue, so the player may have room for the next one: wake it instead of letting
    // it poll. Notified without holding g_b3audio.lock (the player takes that before calling audio_queued()).
    g_b3audio.cv.notify_one();
}

void write_wav(const char* path, const std::vector<int16_t>& pcm);  // below: the DSP path's dump writer

void b3_player() {
    const bool device = plat::audio_open(b3_done);
    log("audio: B3 AudioOut %s (48000 Hz, stereo PCM16)", device ? "device output" : "timer pacing");
    // NX_AUDIO_DUMP=file.wav on the AudioOut path too. The dump used to be wired only into render_submit() (the
    // DSP DeviceSink path), which Bayonetta 3 never runs — it mixes through FMOD into AudioOut — so an A/V audit
    // of this game could not get a WAV at all (av-audit.md F2). Same rule as there: 10 s from the first
    // non-silent buffer on, so the dump is the game's sound and not the boot's leading zeros.
    static const char* dump_path = std::getenv("NX_AUDIO_DUMP");
    std::vector<int16_t> dump;
    using clock = std::chrono::steady_clock;
    // Where the sound went (NX_AUDIO_STATS): buffers the game appended, buffers submitted to the device, and the
    // peak sample of what it did submit — silence with appends flowing is the game's mixer, not the host device.
    uint64_t submitted = 0;
    int peak = 0;
    auto last_stats = clock::now();
    auto next = clock::now();
    for (;;) {
        std::vector<std::shared_ptr<B3Out>> outputs;
        {
            std::unique_lock lk(g_b3audio.lock);
            // Wait only when there is no work: an unconditional delay on each buffer slows the audio clock
            // and stretches audio-paced cutscenes. A queued buffer is not ready if the device queue is full;
            // otherwise this loop spins. b3_done() signals when the device callback actually makes room.
            const auto room = [device] { return !device || plat::audio_queued() < 4; };
            const auto work_ready = [&] {
                if (!room()) return false;
                for (auto& [address, out] : g_b3audio.outputs) {
                    // Queue/state writers hold this mutex. Keep the same registry -> output
                    // lock order as CloseAudioOut while evaluating the wake-up predicate.
                    std::lock_guard output_lock(out->lock);
                    if (out->started && !out->closed && !out->queued.empty()) return true;
                }
                return false;
            };
            if (!work_ready())
                g_b3audio.cv.wait_for(lk, std::chrono::milliseconds(2),
                                      [&] { return g_b3audio.shutdown || work_ready(); });
            if (g_b3audio.shutdown) return;
            for (auto& [address, out] : g_b3audio.outputs) outputs.push_back(out);
        }
        for (auto& out : outputs) {
            if (device && plat::audio_queued() >= 4) break;
            std::unique_ptr<B3Submission> submission;
            {
                std::lock_guard lk(out->lock);
                if (out->closed || !out->started || out->queued.empty()) continue;
                uint64_t buffer = out->queued.front();
                uint64_t data = ld<uint64_t>(buffer + 8), size = ld<uint64_t>(buffer + 0x18);
                submission = std::make_unique<B3Submission>();
                submission->out = out;
                submission->buffer = buffer;
                submission->pcm.resize(size / 2);
                std::memcpy(submission->pcm.data(), reinterpret_cast<const void*>(data), size);
                out->queued.pop_front();
                ++submitted;
                for (size_t k = 0; k < submission->pcm.size(); k += 97) {
                    int s = submission->pcm[k];
                    if (s > peak) peak = s;
                    if (-s > peak) peak = -s;
                }
                // NX_AUDIO_TRACE=1: what the game's mixer (FMOD, inside main) actually handed over: where, how much,
                // and whether it is silence. A silent game-side mix is a game/DSP problem, not the host device.
                static const bool trace = std::getenv("NX_AUDIO_TRACE") != nullptr;
                static int traced = 0;
                if (trace && traced < 24) {
                    size_t nonzero = 0;
                    for (int16_t v : submission->pcm) nonzero += v != 0;
                    log("audio trace #%d: buffer %llx data %llx size %llu (%zu samples, %zu nonzero), ch %u, "
                        "first %d %d %d %d %d %d",
                        traced, (unsigned long long)buffer, (unsigned long long)data, (unsigned long long)size,
                        submission->pcm.size(), nonzero, out->channels,
                        submission->pcm.size() > 0 ? submission->pcm[0] : 0,
                        submission->pcm.size() > 1 ? submission->pcm[1] : 0,
                        submission->pcm.size() > 2 ? submission->pcm[2] : 0,
                        submission->pcm.size() > 3 ? submission->pcm[3] : 0,
                        submission->pcm.size() > 4 ? submission->pcm[4] : 0,
                        submission->pcm.size() > 5 ? submission->pcm[5] : 0);
                    ++traced;
                }
            }
            if (clock::now() - last_stats > std::chrono::seconds(10)) {
                last_stats = clock::now();
                log("audio: %llu buffers appended, %llu submitted, peak sample %d, playback queries %llu",
                    (unsigned long long)g_b3_appends.load(), (unsigned long long)submitted, peak,
                    (unsigned long long)g_b3_playback_queries.load(std::memory_order_relaxed));
            }
            // Ownership is transferred before submit: a device may complete synchronously.
            auto* token = submission.release();
            if (out->channels == 6) {  // 5.1 (FL FR FC LFE BL BR): fold to stereo, keeping 48 kHz PCM16
                std::vector<int16_t>& v = token->pcm;
                const size_t frames = v.size() / 6;
                auto clamp16 = [](int x) { return int16_t(x < -32768 ? -32768 : x > 32767 ? 32767 : x); };
                for (size_t k = 0; k < frames; k++) {
                    const int16_t* s = v.data() + k * 6;
                    // The surround pairs at 0.707: standard Lt/Rt fold-down, the LFE dropped.
                    const int side = (s[2] * 46341) >> 16;  // C * 0.707
                    const int back = (s[4] * 46341) >> 16;  // BL * 0.707
                    v[k * 2] = clamp16(s[0] + side + back);
                    v[k * 2 + 1] = clamp16(s[1] + side + ((s[5] * 46341) >> 16));
                }
                v.resize(frames * 2);
            }
            if (dump_path && dump.size() < 48000 * 2 * 10) {
                bool any = false;
                for (int16_t v : token->pcm) any |= v != 0;
                if (any || !dump.empty()) {
                    dump.insert(dump.end(), token->pcm.begin(), token->pcm.end());
                    if (dump.size() >= 48000 * 2 * 10) write_wav(dump_path, dump);
                }
            }
            const uint64_t frames = token->pcm.size() / 2;
            const auto duration = std::chrono::nanoseconds(frames * 1000000000ull / 48000);
#if defined(__linux__) && !defined(__ANDROID__)
            // Release may happen inside submit. Set the policy before transferring
            // the token; never touch it again after a successful submission.
            token->count_on_release = false;
            if (device && plat::audio_submit_tracked(token->pcm.data(), token->pcm.size() * 2, token,
                                                     out->playback)) continue;
#else
            if (device && plat::audio_submit(token->pcm.data(), token->pcm.size() * 2, token)) continue;
#endif
            token->count_on_release = false;  // timer progress is published after its deadline
            // Timer pacing (no device: headless / NX_MUTE). The deadline is an absolute chain: `next` keeps
            // advancing by the buffer's duration. It used to be re-anchored to "now" on every pass, which
            // turned every oversleep of the condition-variable wait into permanent drift -- measured 12.1 ms
            // per 10.667 ms buffer = 82.6 appends/s (0.88x wall, av-audit.md F9). Re-anchoring is kept only
            // for "more than one buffer behind", where catching up with back-to-back submissions is right.
            // The buffer is handed back before the sleep so the game's mixer produces the next one during it.
            next += duration;
            {
                auto now = clock::now();
                if (now - next > duration) next = now + duration;
            }
            b3_done(token);
            {
                std::unique_lock lk(g_b3audio.lock);
                if (g_b3audio.cv.wait_until(lk, next, [] { return g_b3audio.shutdown; })) return;
            }
            // The guest may already have reused the released buffer. Publish only
            // the elapsed sample frames, to this open instance's retained cursor.
            out->playback->frames.fetch_add(frames, std::memory_order_relaxed);
        }
    }
}

void InitializeAudioOutParameter(CPU& c) {
    // Exact SDK initializer: zero rate/channels and the reserved 0xcafe marker.
    st<uint64_t>(c.x[0], 0xcafe000000000000ull);
}

void OpenDefaultAudioOutNoEvent(CPU& c) {
    const uint64_t address = c.x[0], parameter = c.x[1];
    const uint32_t rate = ld<uint32_t>(parameter);
    const uint16_t channels = ld<uint16_t>(parameter + 4);
    // Six channels (the game's surround branch) is folded to stereo at submit: the host stream is stereo.
    if ((rate && rate != 48000) || (channels && channels != 2 && channels != 6))
        fatal("B3 AudioOut: unsupported output format %u Hz/%u channels", rate, unsigned(channels));
    std::lock_guard lk(g_b3audio.lock);
    if (g_b3audio.outputs.count(address)) { c.x[0] = 0x1299; return; } // sdk+576b0
    if (g_b3audio.outputs.size() >= 2) { c.x[0] = 0xa99; return; } // sdk+5766c, g_AudioOutCountMax
    auto out = std::make_shared<B3Out>();
    out->channels = channels ? channels : 2;
    const uint32_t effective = out->channels;
    g_b3audio.outputs.emplace(address, std::move(out));
    st<uint32_t>(address, 48000);
    st<uint32_t>(address + 4, effective);
    st<uint32_t>(address + 8, 2);  // PCM16; SDK GetSampleByteSize table entry 2 = 2 bytes
    st<uint32_t>(address + 12, 1); // stopped; main+1220f98 stops only when state == 0
    std::memset(reinterpret_cast<void*>(address + 0x10), 0, 0x100);
    std::strcpy(reinterpret_cast<char*>(address + 0x10), "AudioTvOutput");
    st<uint64_t>(address + 0x160, 0);
    if (!g_b3audio.worker.joinable()) g_b3audio.worker = std::thread(b3_player);
    c.x[0] = 0;
}

void RegisterBufferEvent(CPU& c) {
    auto out = b3_out(c.x[0]);
    const uint64_t sysev = c.x[1];
    const uint64_t attach = find_symbol(
        "_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    if (!attach) fatal("B3 AudioOut: AttachReadableHandleToSystemEvent not found");
    if (ld<uint8_t>(sysev + 0x28)) fatal("B3 AudioOut: SystemEvent already initialized");
    auto event = std::make_shared<KEvent>();
    event->state = out->event;
    const uint32_t handle = handle_add(event);
    uint64_t saved[8];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = sysev; c.x[1] = handle; c.x[2] = 1; c.x[3] = 1; // managed, AutoClear (sdk+57fe0)
    try { call(c, attach); }
    catch (...) { handle_close(handle); std::memcpy(c.x, saved, sizeof saved); throw; }
    std::memcpy(c.x, saved, sizeof saved);
    c.x[0] = 0;
}

void OpenDefaultAudioOut(CPU& c) {
    const uint64_t address = c.x[0], sysev = c.x[1];
    c.x[1] = c.x[2];
    OpenDefaultAudioOutNoEvent(c);
    if (c.x[0]) return;
    c.x[0] = address; c.x[1] = sysev;
    RegisterBufferEvent(c);
}

void StartAudioOut(CPU& c) {
    auto out = b3_out(c.x[0]);
    std::lock_guard lk(out->lock);
    out->started = true;
    st<uint32_t>(c.x[0] + 12, 0);
    g_b3audio.cv.notify_all();
    c.x[0] = 0;
}
void StopAudioOut(CPU& c) {
    auto out = b3_out(c.x[0]);
    std::lock_guard lk(out->lock);
    out->started = false;
    st<uint32_t>(c.x[0] + 12, 1);
    c.x[0] = 0;
}
void CloseAudioOut(CPU& c) {
    std::lock_guard lk(g_b3audio.lock);
    auto it = g_b3audio.outputs.find(c.x[0]);
    if (it == g_b3audio.outputs.end()) return; // SDK close is idempotent
    {
        std::lock_guard ol(it->second->lock);
        it->second->closed = true;
        it->second->started = false;
        it->second->queued.clear();
        it->second->released.clear();
        it->second->owned.clear();
        it->second->event->signaled = false;
        st<uint32_t>(c.x[0] + 12, 1);
    }
    g_b3audio.outputs.erase(it); // in-flight tokens retain only the old host object
}
void SetAudioOutBufferInfo(CPU& c) {
    st<uint64_t>(c.x[0] + 8, c.x[1]);
    st<uint64_t>(c.x[0] + 0x10, c.x[2]);
    st<uint64_t>(c.x[0] + 0x18, c.x[3]);
}
void GetAudioOutBufferDataPointer(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0] + 8); }
void GetAudioOutBufferDataSize(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0] + 0x18); }
void GetAudioOutBufferBufferSize(CPU& c) { c.x[0] = ld<uint64_t>(c.x[0] + 0x10); }
void AppendAudioOutBuffer(CPU& c) {
    auto out = b3_out(c.x[0]);
    const uint64_t buffer = c.x[1];
    const uint64_t data = ld<uint64_t>(buffer + 8), capacity = ld<uint64_t>(buffer + 0x10);
    const uint64_t size = ld<uint64_t>(buffer + 0x18), offset = ld<uint64_t>(buffer + 0x20);
    // Unknown offsets/formats must not silently play the wrong memory. The observed callers zero offset.
    if (!data || !size || size > capacity || size % 4 || offset)
        fatal("B3 AudioOut: unsupported/invalid buffer (size %llu capacity %llu offset %llu)",
              (unsigned long long)size, (unsigned long long)capacity, (unsigned long long)offset);
    std::lock_guard lk(out->lock);
    if (!out->owned.insert(buffer).second) { c.x[0] = 0; return; }
    out->queued.push_back(buffer);
    g_b3_appends.fetch_add(1, std::memory_order_relaxed);
    g_b3audio.cv.notify_all();
    c.x[0] = 1;
}
void GetReleasedAudioOutBuffer(CPU& c) {
    auto out = b3_out(c.x[0]);
    std::lock_guard lk(out->lock);
    c.x[0] = 0;
    if (out->released.empty()) return;
    c.x[0] = out->released.front();
    out->released.pop_front();
    out->owned.erase(c.x[0]);
    // AutoClear belongs to the guest wait, not to polling this release queue.
}
void GetAudioOutState(CPU& c) {
    std::lock_guard lk(g_b3audio.lock);
    auto it = g_b3audio.outputs.find(c.x[0]);
    if (it == g_b3audio.outputs.end()) { c.x[0] = ld<uint32_t>(c.x[0] + 12); return; }
    std::lock_guard ol(it->second->lock);
    c.x[0] = it->second->started ? 0 : 1;
}
void GetAudioOutSampleRate(CPU& c) { b3_out(c.x[0]); c.x[0] = 48000; }
void GetAudioOutChannelCount(CPU& c) { b3_out(c.x[0]); c.x[0] = 2; }
void GetAudioOutSampleFormat(CPU& c) { b3_out(c.x[0]); c.x[0] = 2; }
void GetAudioOutPlayedSampleCount(CPU& c) {
    auto out = b3_out(c.x[0]);
    std::lock_guard lk(out->lock);
    c.x[0] = out->playback->frames.load(std::memory_order_relaxed);
    g_b3_playback_queries.fetch_add(1, std::memory_order_relaxed);
}
#endif

void ListAudioIns(CPU& c) { c.x[0] = 0; }
void audio_in_fail(CPU& c) { c.x[0] = (1u << 9) | 153u; }  // audio: not found
void zero(CPU& c) { c.x[0] = 0; }

#if !defined(NX_BAYONETTA3)
// SDK 16 (Super Mario Bros. Wonder): the AudioRenderer. The sdk carries the renderer's server side too and can
// reach it in process ("direct function call") instead of over IPC to audren:u: hand out that one.
// Both return nn::sf::SharedPointer<IAudioRendererManager> through x8.
void CreateAudioRendererManagerByHipc(CPU& c) {
    static uint64_t dfc = find_symbol("_ZN2nn5audio31CreateAudioRendererManagerByDfcEv");
    if (!dfc) fatal("CreateAudioRendererManagerByDfc not found");
    log("audio: AudioRenderer manager in process (DFC) instead of audren:u");
    call(c, dfc);
}

// The AudioRenderer, in process (DFC). On the console the ADSP renders one audio frame every 5 ms and signals the
// renderer's system event, which paces the game's sound thread. In process nothing does that on its own: the
// renderer object (AudioRendererManagerImpl::AudioRendererImpl) has ExecuteAudioRendererRendering, which generates
// the frame's commands and runs the sdk's own DSP command processor on the calling thread, and its render system
// (impl + 0x60) signals its own event, which has no kernel object in process. A guest thread of ours renders every
// 5 ms and signals the game's event itself, then sends the frame's final mix to the host device.
constexpr uint64_t kRendererVtable = 0xd04910;     // sdk: vtable of the sf object wrapping AudioRendererImpl
constexpr uint64_t kExecuteRendering = 0x20cac8;   // sdk: AudioRendererImpl::ExecuteAudioRendererRendering

// The in-process render system hands the client a readable handle of 0 for the renderer's event (there is no
// kernel event behind it), so the SystemEvent the game waits on is ours: an InterProcessEventType keeps its
// readable handle at +0x14 and its writable one at +0x18 (nn::os::detail::WaitInterProcessEvent, sdk+1305a0).
std::shared_ptr<EventState> g_audio_frame = std::make_shared<EventState>();

std::mutex g_renderers_lock;
std::vector<uint64_t> g_renderers;  // AudioRendererImpl*

uint64_t sdk_base() {
    static uint64_t base = [] {
        const LoadedModule* m = module_at(find_symbol("nvnBootstrapLoader"));
        return m ? m->base : 0;
    }();
    return base;
}

// The AudioRendererImpl behind an AudioRendererHandle: an sf object whose vtable pointer is that one, within two
// pointer hops of the handle.
uint64_t find_renderer(uint64_t handle) {
    uint64_t vptr = sdk_base() + kRendererVtable + 0x10;
    auto valid = [](uint64_t p) { return p >= kModuleBase && p < kKernelBase + kKernelSize && (p & 7) == 0; };
    for (uint64_t a = 0; a < 0x40; a += 8) {
        uint64_t p = ld<uint64_t>(handle + a);
        if (!valid(p)) continue;
        if (ld<uint64_t>(p) == vptr) return p + 0x10;
        for (uint64_t b = 0; b < 0x40; b += 8) {
            uint64_t q = ld<uint64_t>(p + b);
            if (valid(q) && ld<uint64_t>(q) == vptr) return q + 0x10;
        }
    }
    return 0;
}

// ---- the renderer's final mix to the host device ----
// The command list ends with a DeviceSink command (id 19) naming the final mix's buffers, but the sdk's DSP command
// processor skips it: on the console the ADSP's sink stage hands those buffers to the output device. So
// CommandListProcessor::Process is wrapped: after the real one has run the list, the sink's input mix buffers (s32
// samples in the PCM16 range, one buffer per channel) are summed into g_render.mix; the render thread converts each
// 5 ms frame to s16 stereo and queues it on the device. Command header: {u32 magic 0xcafebabe; u8 enabled; s8 id;
// s16 size; u32 estimated time}. DeviceSink (CommandBuffer::Generate<DeviceSink>, sdk+211e10): +0x128 the mix
// buffers (or the upsampler's), +0x130 input count, +0x134 s16 buffer index per input.
constexpr uint8_t kCmdDownMix6chTo2ch = 17, kCmdDeviceSink = 19;
constexpr int kFrame = 240;          // samples per 5 ms at 48 kHz
constexpr int kBlocks = 64;          // 320 ms of output blocks
constexpr uint32_t kMaxQueued = 12;  // 60 ms in the device queue: past that, frames are dropped
constexpr size_t kPrime = 4;         // after an underrun, 20 ms are gathered before playing again

struct RenderOut {
    std::mutex lock;
    int32_t mix[kFrame * 2] = {};
    bool mixed = false;
    int16_t blocks[kBlocks][kFrame * 2];
    std::vector<int> free, held;
    bool priming = true;
    // statistics since the last log, and the optional dump (NX_AUDIO_DUMP=file.wav: 10 s from the first sound)
    uint64_t frames = 0, dropped = 0, underruns = 0, silent = 0;
    double sum_sq = 0;
    int peak = 0;
    std::vector<int16_t> dump;
    RenderOut() {
        for (int k = kBlocks - 1; k >= 0; k--) free.push_back(k);
    }
};
RenderOut g_render;

bool render_owns(void* ctx) {
    auto p = static_cast<const uint8_t*>(ctx), b = reinterpret_cast<const uint8_t*>(g_render.blocks);
    return p >= b && p < b + sizeof g_render.blocks;
}

void render_block_done(void* ctx) {
    std::lock_guard lk(g_render.lock);
    g_render.free.push_back(int((static_cast<int16_t*>(ctx) - g_render.blocks[0]) / (kFrame * 2)));
}

void capture_device_sink(uint64_t proc) {  // proc: nn::audio::dsp::CommandListProcessor after Process
    uint64_t cmd = ld<uint64_t>(proc) + 0x20;  // the list header, then the commands (+8 is the cursor: at the end)
    uint32_t count = ld<uint32_t>(proc + 0x1c), samples = ld<uint32_t>(proc + 0x20), rate = ld<uint32_t>(proc + 0x28);
    uint32_t frames = rate == 48000 ? samples : kFrame;  // other rates: the upsampler's 48 kHz buffers
    bool downmixed = false;
    for (uint32_t i = 0; i < count && ld<uint32_t>(cmd) == 0xcafebabe; i++, cmd += ld<int16_t>(cmd + 6)) {
        if (!ld<uint8_t>(cmd + 4)) continue;
        uint8_t id = ld<uint8_t>(cmd + 5);
        if (id == kCmdDownMix6chTo2ch) downmixed = true;
        if (id != kCmdDeviceSink) continue;
        uint64_t buffers = ld<uint64_t>(cmd + 0x128);
        uint32_t inputs = ld<uint32_t>(cmd + 0x130);
        static bool once = false;
        if (!once) {
            once = true;
            log("audio: device sink '%s', %u inputs%s, %u samples at %u Hz", reinterpret_cast<const char*>(cmd + 0x20),
                inputs, downmixed ? " (downmixed to stereo)" : "", samples, rate);
        }
        if (!buffers || !inputs || frames != kFrame) continue;
        auto ch = [&](uint32_t k) {
            return reinterpret_cast<const int32_t*>(buffers + uint64_t(ld<int16_t>(cmd + 0x134 + 2 * k)) * frames * 4);
        };
        const int32_t* l = ch(0);
        const int32_t* r = inputs > 1 ? ch(1) : l;
        // Six channels without the sdk's own downmix (FL FR FC LFE RL RR): fold them to stereo here.
        const int32_t *c = nullptr, *rl = nullptr, *rr = nullptr;
        if (inputs >= 6 && !downmixed) c = ch(2), rl = ch(4), rr = ch(5);
        std::lock_guard lk(g_render.lock);
        for (int s = 0; s < kFrame; s++) {
            int64_t a = l[s], b = r[s];
            if (c) {
                a += (int64_t(c[s]) + rl[s]) * 707 / 1000;
                b += (int64_t(c[s]) + rr[s]) * 707 / 1000;
            }
            g_render.mix[2 * s] += int32_t(std::clamp<int64_t>(a, INT32_MIN, INT32_MAX));
            g_render.mix[2 * s + 1] += int32_t(std::clamp<int64_t>(b, INT32_MIN, INT32_MAX));
        }
        g_render.mixed = true;
    }
}

// AudioRenderSystem::Update (the game's RequestUpdate) takes the render system's mutex, but GenerateCommand runs
// without it: on the console both run on the audio system's side, one after the other. Here the game's sound thread
// updates while our render thread generates, and an effect being updated mid-generation crashed
// (EffectContext::UpdateStateByDspShared on a null effect). Both are serialized by this lock.
std::mutex g_render_system;

// One update per rendered frame, as on the console: an update copies the game's parameters and a new effect's
// "initialize" status rides on exactly one of them. Our render thread signals the game's audio event every 5 ms
// whether or not it rendered since the last update, so a sound thread that fell behind updated twice in a row, the
// second update overwrote the status, and the next render applied a LightLimiter whose state was never initialized
// (null pointers in ApplyLightLimiterEffect, an intermittent crash at startup; docs/nvn/DEBUG.md #28). An update
// now waits (at most 1 s, outside the lock) until a render ran since the previous one.
std::mutex g_pace_lock;
std::condition_variable g_pace_cv;
uint64_t g_renders = 0, g_renders_at_update = ~0ull;

void AudioRenderSystemUpdate(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    {
        std::unique_lock pk(g_pace_lock);
        static uint64_t waited = 0;
        if (g_renders == g_renders_at_update && waited++ < 5) log("audio: update waits for a render");
        // 1 s, not 50 ms: at boot, with every thread loading, a render took longer and the second update went through
        // (the limiter crash again, once in ~25 Windows runs). The render thread runs every 5 ms: 1 s only bounds a stall.
        if (!g_pace_cv.wait_for(pk, std::chrono::seconds(1), [] { return g_renders != g_renders_at_update; }))
            log("audio: no render for 1 s since the last update: updating anyway");
    }
    std::lock_guard lk(g_render_system);
    call(c, real);
    // Counted after the update, under the render lock (renders count themselves under it too): a render that began
    // before this update and ended after it no longer passes for one that used it (the limiter crash, DEBUG-2 #74).
    std::lock_guard pk(g_pace_lock);
    g_renders_at_update = g_renders;
}

void CommandListProcessorProcess(CPU& c) {
    static uint64_t real = find_symbol("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    uint64_t proc = c.x[0];
    call(c, real);
    capture_device_sink(proc);
}

// Called by the render thread after each 5 ms frame.
void render_submit() {
    static const bool device = device_open();
    static const char* dump_path = std::getenv("NX_AUDIO_DUMP");
    std::lock_guard lk(g_render.lock);
    RenderOut& o = g_render;
    if (!o.mixed) return;
    o.mixed = false;
    int16_t pcm[kFrame * 2];
    bool any = false;
    for (int s = 0; s < kFrame * 2; s++) {
        int v = std::clamp(o.mix[s], -32768, 32767);
        pcm[s] = int16_t(v);
        o.mix[s] = 0;
        o.peak = std::max(o.peak, std::abs(v));
        o.sum_sq += double(v) * v;
        any |= v != 0;
    }
    o.silent += !any;
    if (dump_path && o.dump.size() < 48000 * 2 * 10 && (any || !o.dump.empty())) {
        o.dump.insert(o.dump.end(), pcm, pcm + kFrame * 2);
        if (o.dump.size() >= 48000 * 2 * 10) write_wav(dump_path, o.dump);
    }
    if (++o.frames % 2000 == 0) {
        log("audio: mix out: peak %d, rms %.0f, %llu of 2000 frames silent, %llu dropped, %llu underruns, queue %u",
            o.peak, std::sqrt(o.sum_sq / (2000.0 * kFrame * 2)), (unsigned long long)o.silent,
            (unsigned long long)o.dropped, (unsigned long long)o.underruns, device ? plat::audio_queued() : 0);
        o.peak = 0, o.sum_sq = 0, o.silent = o.dropped = o.underruns = 0;
    }
    if (!device) return;
    uint32_t queued = plat::audio_queued();
    if (queued + o.held.size() >= kMaxQueued || o.free.empty()) {  // the device fell behind: keep latency bounded
        o.dropped++;
        return;
    }
    if (queued == 0 && !o.priming) o.priming = true, o.underruns++;
    int k = o.free.back();
    o.free.pop_back();
    std::memcpy(o.blocks[k], pcm, sizeof pcm);
    o.held.push_back(k);
    if (o.priming && o.held.size() < kPrime) return;
    o.priming = false;
    for (int h : o.held) plat::audio_submit(o.blocks[h], sizeof pcm, o.blocks[h]);
    o.held.clear();
}

void audio_render_thread(CPU& c) {
    log("audio: render thread running");
    uint64_t next = plat::monotonic_ns();
    for (;;) {
        next += 5'000'000;
        int64_t wait = int64_t(next - plat::monotonic_ns());
        if (wait > 0) plat::sleep_ns(wait);
        std::vector<uint64_t> list;
        {
            std::lock_guard lk(g_renderers_lock);
            list = g_renderers;
        }
        static uint64_t frames = 0, busy_ns = 0, last_rc = 0;
        uint64_t t0 = plat::monotonic_ns();
        for (uint64_t impl : list) {
            uint64_t saved[31];
            std::memcpy(saved, c.x, sizeof saved);
            c.x[0] = impl;
            std::lock_guard lk(g_render_system);
            call(c, sdk_base() + kExecuteRendering);
            last_rc = c.x[0];
            std::memcpy(c.x, saved, sizeof saved);
            std::lock_guard pk(g_pace_lock);  // counted under the render lock (AudioRenderSystemUpdate)
            g_renders++;
            g_pace_cv.notify_all();
        }
        busy_ns += plat::monotonic_ns() - t0;
        render_submit();
        if (++frames % 2000 == 0)
            log("audio: %llu frames rendered, %.1f us each, last result %llx", (unsigned long long)frames,
                double(busy_ns) / 1000.0 / 2000.0, (unsigned long long)last_rc), busy_ns = 0;
        g_audio_frame->signaled = true;
        notify_all();
    }
}

void start_render_thread(CPU& c) {
    uint64_t create = find_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    uint64_t start = find_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    if (!create || !start) {
        log("audio: nn::os::CreateThread/StartThread not found, no render thread");
        return;
    }
    uint64_t thread = kernel_alloc(0x1000), stack = kernel_alloc(0x40000);
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    c.x[0] = thread, c.x[1] = hle_address("__audio_render_thread"), c.x[2] = 0, c.x[3] = stack, c.x[4] = 0x40000;
    c.x[5] = 16;  // high priority, like the audio system's own threads
    call(c, create);
    uint64_t rc = c.x[0];
    c.x[0] = thread;
    if (rc == 0) call(c, start);
    std::memcpy(c.x, saved, sizeof saved);
    if (rc) log("audio: CreateThread failed (%llx)", (unsigned long long)rc);
}

// OpenAudioRenderer(AudioRendererHandle*, os::SystemEvent*, const AudioRendererParameter&, void* work, size_t)
void OpenAudioRenderer(CPU& c) {
    static uint64_t real = find_symbol(
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    uint64_t handle = c.x[0], sysev = c.x[1];
    call(c, real);
    if (c.x[0] == 0 && ld<uint32_t>(sysev + 0x14) == 0) {
        auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
        r->state = w->state = g_audio_frame;
        st<uint32_t>(sysev + 0x14, handle_add(r));
        st<uint32_t>(sysev + 0x18, handle_add(w));
    }
    uint64_t rc = c.x[0];
    uint64_t impl = rc == 0 ? find_renderer(handle) : 0;
    log("audio: renderer opened (result %llx), impl %llx", (unsigned long long)rc, (unsigned long long)impl);
    if (!impl) return;
    // ExecuteAudioRendererRendering only renders in the manual execution mode on the CPU rendering device
    // (AudioRenderSystem::GetExecutionMode / GetRenderingDevice == 1, at render system + 0x2d0 / + 0x2d4); the game
    // opens the renderer in the automatic mode, where the ADSP renders on its own. Our 5 ms thread is that ADSP, so
    // the renderer is switched to manual/CPU; otherwise every call failed with 0x40299 and nothing was ever rendered
    // (streamed music never advanced, and the opening cutscene waited for it forever).
    uint64_t system = impl + 0x60;
    log("audio: execution mode %u, rendering device %u -> 1, 1", ld<uint32_t>(system + 0x2d0), ld<uint32_t>(system + 0x2d4));
    st<uint32_t>(system + 0x2d0, 1);
    st<uint32_t>(system + 0x2d4, 1);
    {
        std::lock_guard lk(g_renderers_lock);
        g_renderers.push_back(impl);
    }
    static bool started = false;
    if (!started) {
        started = true;
        start_render_thread(c);
    }
    c.x[0] = rc;
}

#else
// B3's renderer ABI and implementation offsets have not been established. Never enter Wonder's
// in-process renderer, even in diagnostic mode; fail at the actual audio call with its symbol.
[[noreturn]] void unsupported_renderer(CPU& c) {
    fatal("B3 AudioRenderer is not implemented: %s (lr %s); Wonder SDK offsets are disabled",
          hle_current_name(), describe(c.x[30]).c_str());
}
void CreateAudioRendererManagerByHipc(CPU& c) { unsupported_renderer(c); }
void OpenAudioRenderer(CPU& c) { unsupported_renderer(c); }
void audio_render_thread(CPU& c) { unsupported_renderer(c); }
void CommandListProcessorProcess(CPU& c) { unsupported_renderer(c); }
void AudioRenderSystemUpdate(CPU& c) { unsupported_renderer(c); }
#endif

// The audio device (output) side: one stereo output that never changes.
constexpr const char* kOutputName = "AudioTvOutput";

void AcquireAudioDeviceNotificationForOutput(CPU& c) {  // (os::SystemEvent*): an event that is never signaled
    auto w = std::make_shared<KEvent>(), r = std::make_shared<KEvent>();
    r->state = w->state = std::make_shared<EventState>();
    uint32_t hr = handle_add(r), hw = handle_add(w);
    uint64_t attach = find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;  // x0: the SystemEvent's SystemEventType
    call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = 0;
}

void ListAudioOutputDeviceName(CPU& c) {  // int (AudioDeviceName* names, int count): 256 chars each
    if (int(c.x[1]) < 1) {
        c.x[0] = 0;
        return;
    }
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 1;
}

void GetActiveAudioOutputDeviceName(CPU& c) {  // (AudioDeviceName*)
    std::memset(reinterpret_cast<void*>(c.x[0]), 0, 256);
    std::strcpy(reinterpret_cast<char*>(c.x[0]), kOutputName);
    c.x[0] = 0;
}

void two(CPU& c) { c.x[0] = 2; }

}  // namespace

NX_HLE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", AcquireAudioDeviceNotificationForOutput);
NX_HLE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", ListAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", GetActiveAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", zero);
NX_HLE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", two);
NX_HLE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", InitializeAudioOutParameter);
NX_HLE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", OpenDefaultAudioOut);
#if defined(NX_BAYONETTA3)
// Both B3 main and subsdk0 import this overload; the legacy HLE requires a SystemEvent argument.
NX_HLE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", OpenDefaultAudioOutNoEvent);
NX_HLE("_ZN2nn5audio19RegisterBufferEventEPNS0_8AudioOutEPNS_2os11SystemEventE", RegisterBufferEvent);
NX_HLE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", GetAudioOutState);
NX_HLE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", GetAudioOutSampleRate);
NX_HLE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", GetAudioOutChannelCount);
NX_HLE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", GetAudioOutSampleFormat);
NX_HLE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", GetAudioOutBufferDataSize);
NX_HLE("_ZN2nn5audio27GetAudioOutBufferBufferSizeEPKNS0_14AudioOutBufferE", GetAudioOutBufferBufferSize);
NX_HLE("_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE", GetAudioOutPlayedSampleCount);
NX_HLE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", ListAudioOutputDeviceName);
NX_HLE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", GetActiveAudioOutputDeviceName);
#endif
NX_HLE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", StartAudioOut);
NX_HLE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", StopAudioOut);
NX_HLE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", CloseAudioOut);
NX_HLE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", SetAudioOutBufferInfo);
NX_HLE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", GetAudioOutBufferDataPointer);
NX_HLE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", AppendAudioOutBuffer);
NX_HLE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", GetReleasedAudioOutBuffer);
NX_HLE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", ListAudioIns);
NX_HLE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", audio_in_fail);
NX_HLE("_ZN2nn5audio26InitializeAudioInParameterEPNS0_16AudioInParameterE", zero);
NX_HLE("_ZN2nn5audio32CreateAudioRendererManagerByHipcEv", CreateAudioRendererManagerByHipc);
NX_HLE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", OpenAudioRenderer);
NX_HLE("__audio_render_thread", audio_render_thread);
NX_HLE("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv", CommandListProcessorProcess);
NX_HLE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", AudioRenderSystemUpdate);

}  // namespace nx
