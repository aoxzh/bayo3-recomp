// Android hardware video decode for the cutscene path (movie.cpp).
//
// movie.cpp's faked nvdec needs somebody to produce NV12 frames. On a desktop host it forks ffmpeg, which an APK
// cannot do: there is no ffmpeg binary, no /usr/bin, and no PATH to search. Android does have a hardware VP9/AV1
// decoder behind libmediandk, keyed by the same containers, so this is the Android half of the same seam: same
// "decode one movie into NV12 planes" job, a different engine behind it.
//
// Everything here runs on movie.cpp's reader thread, one Stream per open movie. The runtime links libmediandk only
// in the Android build (CMakeLists.txt), so no other platform sees this file.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx::movie_android {

// Container header only, no decode: the same contract as movie.cpp's ffprobe probe(). False means "cannot tell",
// and movie.cpp then leaves playback blank exactly as it does today.
bool probe(const std::string& host_path, uint32_t& w, uint32_t& h, double& fps);

// One decoding session over a romfs movie. Frames come back tightly packed (no row padding): w*h luma bytes
// followed by w*h/2 interleaved chroma bytes, scaled to the size the caller asked for when the stream's own size
// differs.
class Stream {
  public:
    virtual ~Stream() = default;
    // Blocks until a frame is ready. False at the end of the stream, on a decoder error, or once stop() is called.
    virtual bool next(std::vector<uint8_t>& nv12) = 0;
    // Rewinds to the first frame, for the looping title/background movies. False when the container cannot seek.
    virtual bool rewind() = 0;
    // Ends the session from any thread; a next() already in flight returns false promptly.
    virtual void stop() = 0;
    // What the decoder actually produced, for the log line (may differ from what was asked for).
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
};

// Null when the file has no usable video track, or this device has no decoder for its codec.
std::unique_ptr<Stream> open(const std::string& host_path, uint32_t want_w, uint32_t want_h);

}  // namespace nx::movie_android
