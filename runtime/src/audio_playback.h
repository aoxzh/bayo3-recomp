// Playback progress for one audio output. Frames count source stereo S16 frames
// handed off by SDL, rather than guest buffer releases or exact DAC playback.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nx::plat {

struct AudioPlayback {
    std::atomic<uint64_t> frames{0};
};

#if defined(__linux__) && !defined(__ANDROID__)
// Release callbacks retain their existing meaning: the backend no longer needs
// the source buffer. Playback is tracked separately; failed puts are not played.
bool audio_submit_tracked(const void* data, size_t bytes, void* ctx, std::shared_ptr<AudioPlayback> playback);
#endif

}  // namespace nx::plat
