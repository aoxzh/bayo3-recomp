// Windows hardware video decode for the cutscene path: the Media Foundation half of the same seam as
// movie_android.h.
//
// movie.cpp's faked nvdec needs somebody to produce NV12 frames. On Linux and macOS it forks ffmpeg; on Windows,
// Media Foundation and a compatible VP9 transform (such as MSVP9DEC.dll) can decode WebM/VP9 through
// IMFSourceReader. Decoder availability depends on the host configuration.
//
// This is therefore the third engine behind the same "decode one movie into NV12 planes" job, alongside ffmpeg
// (desktop POSIX) and MediaCodec (Android). movie.cpp's open/respawn/close logic is unchanged.
//
// Two details of IMFSourceReader matter for the contract movie.cpp expects:
//   * MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING makes the reader convert to NV12 and scale for us, so the
//     want_w/want_h rescale the Android path does by hand is free here.
//   * Samples carry their own row padding. The reader reports a contiguous buffer whose stride is the frame width
//     for NV12 on this transform, but the stride is read per buffer rather than assumed.
// Everything here runs on movie.cpp's reader thread, one Stream per open movie.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx::movie_win {

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
    virtual bool next(std::vector<uint8_t>& nv12, int64_t& pts_us) = 0;
    // Rewinds to the first frame, for the looping title/background movies. False when the container cannot seek.
    virtual bool rewind() = 0;
    // Ends the session from any thread; a next() already in flight returns false promptly.
    virtual void stop() = 0;
    // What the decoder actually produced, for the log line (may differ from what was asked for).
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
};

// Null when the file has no usable video track, or this machine has no decoder for its codec.
std::unique_ptr<Stream> open(const std::string& host_path, uint32_t want_w, uint32_t want_h);

}  // namespace nx::movie_win
