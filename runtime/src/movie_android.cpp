// Android hardware video decode for the cutscene path: the libmediandk half of movie_android.h.
//
// The shape of the job is fixed by movie.cpp: produce NV12 (w*h luma, then w*h/2 interleaved chroma) and hand it
// over one frame at a time. MediaCodec produces exactly that layout in ByteBuffer mode — the output format reports
// COLOR_FormatYUV420SemiPlanar (0x15) for these VP9 movies — so the work here is (a) driving the extractor/codec
// loop, (b) removing the codec's row padding, and (c) rescaling when the caller asked for a different size.
//
// Device behavior can vary:
//   * AMediaExtractor_setDataSource(path) may fail, so the file is opened and handed to setDataSourceFd instead.
//   * Output format changes can alter the padded stride and slice height; honor the current format for each buffer.
#include "movie_android.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <nx/runtime.h>

namespace nx::movie_android {
namespace {

constexpr uint32_t kColorFormatSemiPlanar = 0x15;  // COLOR_FormatYUV420SemiPlanar: NV12

// The container's own frame rate, used only for movie.cpp's wall-clock pacing. These movies are 30 fps and most
// webm tracks do not carry a rational rate, so 30 is the honest fallback rather than a divide-by-zero.
double read_fps(AMediaFormat* f) {
    int32_t num = 0;
    if (!AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, &num) || num <= 0) return 30.0;
    return double(num);
}

// Nearest-neighbour is enough here: this only runs when the guessed plane size was wrong, and the alternative
// (a scaler dependency on Android) is not worth it for one corrective path per movie.
void blit_plane(uint8_t* dst, uint32_t dst_w, uint32_t dst_h, const uint8_t* src, uint32_t src_w, uint32_t src_h,
                uint32_t src_stride, uint32_t pixel_stride, uint32_t src_offset) {
    for (uint32_t y = 0; y < dst_h; y++) {
        const uint32_t sy = src_h == dst_h ? y : (uint32_t)(uint64_t(y) * src_h / dst_h);
        const uint8_t* row = src + size_t(sy) * src_stride;
        for (uint32_t x = 0; x < dst_w; x++) {
            const uint32_t sx = src_w == dst_w ? x : (uint32_t)(uint64_t(x) * src_w / dst_w);
            dst[size_t(y) * dst_w + x] = row[size_t(sx) * pixel_stride + src_offset];
        }
    }
}

class MediaCodecStream final : public Stream {
  public:
    MediaCodecStream() = default;
    ~MediaCodecStream() override { close(); }

    bool start(const std::string& host_path, uint32_t want_w, uint32_t want_h) {
        const int fd = ::open(host_path.c_str(), O_RDONLY);
        if (fd < 0) {
            log_line("open failed (errno %d)", errno);
            return false;
        }
        struct stat sb {};
        if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
            ::close(fd);
            log_line("fstat failed or empty file");
            return false;
        }
        ex_ = AMediaExtractor_new();
        if (!ex_) {
            ::close(fd);
            return false;
        }
        // setDataSourceFd, not setDataSource: the path-based call returns -10002 on this device.
        const media_status_t ext = AMediaExtractor_setDataSourceFd(ex_, fd, 0, off64_t(sb.st_size));
        ::close(fd);  // the extractor dup()s what it needs
        if (ext != AMEDIA_OK) {
            log_line("setDataSourceFd failed (%d)", int(ext));
            return false;
        }

        AMediaFormat* track = nullptr;
        for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex_); i++) {
            AMediaFormat* f = AMediaExtractor_getTrackFormat(ex_, i);
            const char* mime = nullptr;
            if (f) AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
            if (mime && std::strncmp(mime, "video/", 6) == 0) {
                const media_status_t sel = AMediaExtractor_selectTrack(ex_, i);
                if (sel == AMEDIA_OK) {
                    track = f;
                    break;
                }
            }
            if (f) AMediaFormat_delete(f);
        }
        if (!track) {
            log_line("no video track");
            return false;
        }
        const char* mime = nullptr;
        AMediaFormat_getString(track, AMEDIAFORMAT_KEY_MIME, &mime);
        int32_t coded_w = 0, coded_h = 0;
        AMediaFormat_getInt32(track, AMEDIAFORMAT_KEY_WIDTH, &coded_w);
        AMediaFormat_getInt32(track, AMEDIAFORMAT_KEY_HEIGHT, &coded_h);
        if (coded_w <= 0 || coded_h <= 0) {
            log_line("track has no usable size (%dx%d)", coded_w, coded_h);
            AMediaFormat_delete(track);
            return false;
        }
        src_w_ = uint32_t(coded_w);
        src_h_ = uint32_t(coded_h);
        out_w_ = want_w ? want_w : src_w_;
        out_h_ = want_h ? want_h : src_h_;
        fps_ = read_fps(track);
        codec_ = AMediaCodec_createDecoderByType(mime);
        if (!codec_) {
            log_line("no decoder for %s", mime ? mime : "?");
            AMediaFormat_delete(track);
            return false;
        }
        // No surface: ByteBuffer output is what gives us CPU-readable NV12 on this device.
        const media_status_t cfg = AMediaCodec_configure(codec_, track, nullptr, nullptr, 0);
        AMediaFormat_delete(track);
        if (cfg != AMEDIA_OK) {
            log_line("configure failed (%d)", int(cfg));
            return false;
        }
        if (AMediaCodec_start(codec_) != AMEDIA_OK) {
            log_line("start failed");
            return false;
        }
        started_ = true;
        log_line("decoding %s: %ux%u -> requested %ux%u @ %.3f fps", host_path.c_str(), src_w_, src_h_, out_w_,
                 out_h_, fps_);
        return true;
    }

    bool next(std::vector<uint8_t>& nv12, int64_t& pts_us) override {
        for (int guard = 0; guard < 6000; guard++) {
            if (stopped_.load(std::memory_order_relaxed)) return false;
            if (!input_done_) feed_input();
            if (stopped_.load(std::memory_order_relaxed)) return false;
            AMediaCodecBufferInfo info{};
            const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec_, &info, 5000);
            if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                refresh_format();
                continue;
            }
            if (ob == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED || ob == AMEDIACODEC_INFO_TRY_AGAIN_LATER) continue;
            if (ob < 0) return false;
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                AMediaCodec_releaseOutputBuffer(codec_, size_t(ob), false);
                return false;
            }
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, size_t(ob), &cap);
            const bool ok = buf && convert(buf, cap, nv12);
            pts_us = int64_t(info.presentationTimeUs);
            AMediaCodec_releaseOutputBuffer(codec_, size_t(ob), false);  // no render: this is not a screen
            if (ok) return true;
            // A frame we cannot lay out is skipped rather than ending playback: the next one may be fine.
        }
        return false;
    }

    bool rewind() override {
        if (!ex_ || !codec_) return false;
        if (AMediaExtractor_seekTo(ex_, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC) != AMEDIA_OK) return false;
        AMediaCodec_flush(codec_);
        input_done_ = false;
        return true;
    }

    void stop() override {
        stopped_.store(true, std::memory_order_relaxed);
        // Stopping the codec is what wakes a next() blocked in dequeueOutputBuffer. It runs once: Session::stop()
        // calls this and then destroys the stream, so close() must not stop it again.
        if (started_) {
            started_ = false;
            AMediaCodec_stop(codec_);
        }
    }

    uint32_t width() const override { return src_w_; }
    uint32_t height() const override { return src_h_; }

  private:
    void log_line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);
        nx::log("movie(android): %s", msg);
    }

    void feed_input() {
        const ssize_t ib = AMediaCodec_dequeueInputBuffer(codec_, 5000);
        if (ib < 0) return;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, size_t(ib), &cap);
        const ssize_t n = AMediaExtractor_readSampleData(ex_, buf, cap);
        const int64_t pts = AMediaExtractor_getSampleTime(ex_);
        if (n < 0) {
            AMediaCodec_queueInputBuffer(codec_, size_t(ib), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            input_done_ = true;
            return;
        }
        AMediaCodec_queueInputBuffer(codec_, size_t(ib), 0, size_t(n), uint64_t(pts < 0 ? 0 : pts), 0);
        AMediaExtractor_advance(ex_);
    }

    void refresh_format() {
        AMediaFormat* of = AMediaCodec_getOutputFormat(codec_);
        if (!of) return;
        int32_t cf = -1, w = 0, h = 0, stride = 0, slice = 0;
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_COLOR_FORMAT, &cf);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_HEIGHT, &h);
        if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_STRIDE, &stride) || stride <= 0) stride = w;
        if (!AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice) || slice <= 0) slice = h;
        AMediaFormat_delete(of);
        stride_ = uint32_t(stride > 0 ? stride : 0);
        slice_ = uint32_t(slice > 0 ? slice : 0);
        // The decoder's own size can differ from the track's after a format change (e.g. a crop); keep the stream's
        // coded size for layout and only trust stride/slice for padding.
        if (w > 0 && h > 0) {
            src_w_ = uint32_t(w);
            src_h_ = uint32_t(h);
        }
        if (cf >= 0 && uint32_t(cf) != kColorFormatSemiPlanar)
            log_line("warning: color format 0x%x is not semi-planar; layout may be wrong", unsigned(cf));
        log_line("output %ux%u stride %u slice %u (color 0x%x)", src_w_, src_h_, stride_, slice_, unsigned(cf));
    }

    bool convert(const uint8_t* buf, size_t cap, std::vector<uint8_t>& nv12) {
        const uint32_t stride = stride_ ? stride_ : src_w_;
        const uint32_t slice = slice_ ? slice_ : src_h_;
        const size_t luma_needed = size_t(stride) * size_t(slice);
        if (cap < luma_needed) return false;
        const uint8_t* y = buf;
        // Chroma follows the luma slab. Its row count and stride are the half-size of the luma ones, but the
        // decoder may keep the luma stride for the UV rows; treat the slab as stride*slice/2 bytes, which covers
        // both conventions, and read a UV pair with the luma stride of width/2 pairs.
        const uint8_t* uv = buf + luma_needed;
        const size_t uv_needed = luma_needed / 2;
        if (cap < luma_needed + uv_needed) return false;

        const bool same = src_w_ == out_w_ && src_h_ == out_h_;
        nv12.resize(size_t(out_w_) * out_h_ * 3 / 2);
        if (same && stride == src_w_ && slice == src_h_) {
            // The common case: already tight NV12, one copy.
            std::memcpy(nv12.data(), y, size_t(src_w_) * src_h_);
            std::memcpy(nv12.data() + size_t(src_w_) * src_h_, uv, size_t(src_w_) * src_h_ / 2);
            return true;
        }
        blit_plane(nv12.data(), out_w_, out_h_, y, src_w_, src_h_, stride, 1, 0);
        // The UV slab's rows hold width/2 interleaved pairs, so a "row" is `stride` bytes for the luma width.
        blit_plane(nv12.data() + size_t(out_w_) * out_h_, out_w_ / 2, out_h_ / 2, uv, src_w_ / 2, src_h_ / 2, stride,
                   2, 0);
        blit_plane(nv12.data() + size_t(out_w_) * out_h_ + 1, out_w_ / 2, out_h_ / 2, uv, src_w_ / 2, src_h_ / 2,
                   stride, 2, 1);
        return true;
    }

    void close() {
        stopped_.store(true, std::memory_order_relaxed);
        if (codec_) {
            if (started_) AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
            codec_ = nullptr;
        }
        if (ex_) {
            AMediaExtractor_delete(ex_);
            ex_ = nullptr;
        }
    }

    AMediaExtractor* ex_ = nullptr;
    AMediaCodec* codec_ = nullptr;
    bool started_ = false;
    bool input_done_ = false;
    uint32_t src_w_ = 0, src_h_ = 0, out_w_ = 0, out_h_ = 0;
    uint32_t stride_ = 0, slice_ = 0;
    double fps_ = 30.0;
    std::atomic<bool> stopped_{false};
};

}  // namespace

bool probe(const std::string& host_path, uint32_t& w, uint32_t& h, double& fps) {
    const int fd = ::open(host_path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat sb {};
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
        ::close(fd);
        return false;
    }
    AMediaExtractor* ex = AMediaExtractor_new();
    if (!ex) {
        ::close(fd);
        return false;
    }
    const media_status_t st = AMediaExtractor_setDataSourceFd(ex, fd, 0, off64_t(sb.st_size));
    ::close(fd);
    if (st != AMEDIA_OK) {
        AMediaExtractor_delete(ex);
        return false;
    }
    bool ok = false;
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(ex, i);
        if (!f) continue;
        const char* mime = nullptr;
        AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
        int32_t tw = 0, th = 0;
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &tw);
        AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &th);
        if (mime && std::strncmp(mime, "video/", 6) == 0 && tw > 0 && th > 0) {
            w = uint32_t(tw);
            h = uint32_t(th);
            fps = read_fps(f);
            ok = true;
            AMediaFormat_delete(f);
            break;
        }
        AMediaFormat_delete(f);
    }
    AMediaExtractor_delete(ex);
    return ok;
}

std::unique_ptr<Stream> open(const std::string& host_path, uint32_t want_w, uint32_t want_h) {
    auto s = std::make_unique<MediaCodecStream>();
    if (!s->start(host_path, want_w, want_h)) return nullptr;
    return s;
}

}  // namespace nx::movie_android
