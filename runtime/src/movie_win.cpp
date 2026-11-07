// Windows video decode for the cutscene path: the Media Foundation half of movie_win.h.
//
// movie.cpp hands off one frame at a time as NV12 (w*h luma, then w*h/2 interleaved chroma) and paces the ring
// with the guest's own presentation time. On Linux/macOS that job goes to an ffmpeg subprocess; Android does it
// through MediaCodec. Windows has no ffmpeg on PATH, so this uses Media Foundation with the VP9 Video Extensions
// transform (MSVP9DEC.dll) that ships with Windows 10/11 — measured on the porting machine: ev0000.webm
// (1600x900 VP9) decodes to NV12 at its real 30.10 fps.
//
// Three Media Foundation behaviours are load bearing here, each measured rather than assumed:
//   * MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING is what enables the NV12 conversion AND the scaling: with it, a
//     source reader can be asked for an output size different from the stream's and the video processor does the
//     resize. That is why there is no hand-written blit_plane here, unlike movie_android.cpp.
//   * Samples are not guaranteed tightly packed: the locked buffer's real stride is bytes / (h * 3 / 2) for NV12,
//     and that stride can exceed the width. It is therefore read per frame and rows are copied row by row.
//   * ReadSample() only blocks for as long as one frame takes to decode from a local file, so stop() is a flag
//     checked between frames rather than a cross-thread wakeup (which the source reader does not support).
#include "movie_win.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <nx/runtime.h>

namespace nx::movie_win {
namespace {

// MFStartup is process-wide and refcounted internally, but the source reader is not thread-safe, and every
// session uses its own thread, so COM is initialised per thread in MTA.
std::once_flag g_mf_once;
bool g_mf_ok = false;

void ensure_mf() {
    std::call_once(g_mf_once, [] {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        g_mf_ok = SUCCEEDED(hr);
        if (!g_mf_ok) nx::log("movie(win): MFStartup failed (0x%08lx); cutscenes stay blank", (unsigned long)hr);
    });
}

// One CoInitializeEx per thread that touches the reader. RPC_E_CHANGED_MODE means the thread already joined
// another apartment, which is fine: the reader only needs COM present, not a particular model.
void ensure_com() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), out.data(), n);
    return out;
}

// Open a source reader asking for NV12 at `want_w` x `want_h` (0 x 0 means "the stream's own size"). Returns null
// when the file has no decodable video stream on this machine.
IMFSourceReader* open_reader(const std::string& host_path, uint32_t want_w, uint32_t want_h) {
    ensure_mf();
    if (!g_mf_ok) return nullptr;
    ensure_com();

    IMFAttributes* attrs = nullptr;
    if (FAILED(MFCreateAttributes(&attrs, 2)) || !attrs) return nullptr;
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    IMFSourceReader* reader = nullptr;
    const std::wstring wpath = widen(host_path);
    HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
    attrs->Release();
    if (FAILED(hr) || !reader) {
        nx::log("movie(win): Media Foundation cannot open %s (0x%08lx); an N edition of Windows needs the Media "
                "Feature Pack", host_path.c_str(), (unsigned long)hr);
        return nullptr;
    }

    IMFMediaType* want = nullptr;
    hr = MFCreateMediaType(&want);
    if (FAILED(hr) || !want) {
        reader->Release();
        return nullptr;
    }
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (want_w && want_h) MFSetAttributeSize(want, MF_MT_FRAME_SIZE, want_w, want_h);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
    want->Release();
    if (FAILED(hr)) {  // no NV12 for this stream (unknown codec, or the VP9 extension is missing)
        static bool said = false;
        if (!said)
            nx::log("movie(win): no VP9 decoder for %s (0x%08lx): cutscenes stay blank until the \"VP9 Video "
                    "Extensions\" from the Microsoft Store are installed", host_path.c_str(), (unsigned long)hr),
                said = true;
        reader->Release();
        return nullptr;
    }
    return reader;
}

// The size and rate the reader will actually deliver, after any scaling it agreed to.
bool reader_format(IMFSourceReader* reader, uint32_t& w, uint32_t& h, double& fps) {
    IMFMediaType* got = nullptr;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got)) || !got) return false;
    UINT32 width = 0, height = 0, num = 0, den = 0;
    const bool sized = SUCCEEDED(MFGetAttributeSize(got, MF_MT_FRAME_SIZE, &width, &height));
    const bool rated = SUCCEEDED(MFGetAttributeRatio(got, MF_MT_FRAME_RATE, &num, &den));
    got->Release();
    if (!sized || width == 0 || height == 0) return false;
    w = width;
    h = height;
    // Most WebM tracks carry no rational rate; 30 fps is this game's movie rate and avoids a divide by zero.
    fps = (rated && den != 0 && num != 0) ? double(num) / double(den) : 30.0;
    return true;
}

// The container's nominal rate is not trustworthy for these files: every movie in this game reports
// MF_MT_FRAME_RATE 15/1 while actually delivering frames every 1/30 s (measured: ev0000/ev0005/ev0010 all give
// 59 frame steps spanning 1.967 s = 29.99 fps). movie.cpp uses this number to pace its frame ring, so a nominal 15
// would play the movie at half speed and drop every other frame. The rate is therefore measured from the samples
// themselves, and the header value is only the fallback when the stream yields too few timestamps to tell.
double measure_rate(IMFSourceReader* reader, double fallback) {
    LONGLONG prev = 0;
    bool have_prev = false;
    std::vector<int64_t> deltas;
    for (int i = 0; i < 6 && deltas.size() < 4; i++) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        const HRESULT hr =
            reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (sample) sample->Release();
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!sample) continue;
        if (have_prev && ts > prev) deltas.push_back(ts - prev);
        prev = ts;
        have_prev = true;
    }
    if (deltas.empty()) return fallback;
    // The median delta ignores the odd timestamp jump at a stream's start.
    std::sort(deltas.begin(), deltas.end());
    const int64_t delta = deltas[deltas.size() / 2];
    if (delta <= 0) return fallback;
    return 1e7 / double(delta);  // Media Foundation time units are 100 ns
}

// Nearest-neighbour is enough for the corrective path only: the reader already scales to whatever size was asked
// for, so this runs only if the transform produced something else.
void rescale_nv12(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, std::vector<uint8_t>& dst,
                  uint32_t dw, uint32_t dh) {
    dst.resize(size_t(dw) * dh * 3 / 2);
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t sy = sh == dh ? y : (uint32_t)(uint64_t(y) * sh / dh);
        const uint8_t* srow = src.data() + size_t(sy) * sw;
        uint8_t* drow = dst.data() + size_t(y) * dw;
        for (uint32_t x = 0; x < dw; x++) drow[x] = srow[sw == dw ? x : (uint32_t)(uint64_t(x) * sw / dw)];
    }
    const uint32_t sch = sh / 2, dch = dh / 2;
    for (uint32_t y = 0; y < dch; y++) {
        const uint32_t sy = sch == dch ? y : (uint32_t)(uint64_t(y) * sch / dch);
        const uint8_t* srow = src.data() + size_t(sw) * sh + size_t(sy) * sw;
        uint8_t* drow = dst.data() + size_t(dw) * dh + size_t(y) * dw;
        for (uint32_t x = 0; x < dw; x++) drow[x] = srow[sw == dw ? x : (uint32_t)(uint64_t(x) * sw / dw)];
    }
}

class MfStream final : public Stream {
  public:
    MfStream(std::string host_path, uint32_t want_w, uint32_t want_h)
        : host_(std::move(host_path)), want_w_(want_w), want_h_(want_h) {}
    ~MfStream() override { stop(); }

    // First call does the actual open, on the reader thread: the source reader is used from exactly one thread
    // for its whole life, and movie.cpp's stop() -> join() relies on that.
    bool next(std::vector<uint8_t>& nv12, int64_t& pts_us) override {
        if (stopped_.load(std::memory_order_relaxed)) return false;
        if (!reader_) {
            IMFSourceReader* r = open_reader(host_, want_w_, want_h_);
            if (!r) return false;
            reader_ = r;
            if (!reader_format(reader_, w_, h_, fps_)) {
                stop();
                return false;
            }
            if (want_w_ && want_h_ && (w_ != want_w_ || h_ != want_h_)) scaled_ = true;
            nx::log("movie(win): Media Foundation opened %s at %ux%u @ %.3f fps (asked %ux%u)", host_.c_str(), w_,
                    h_, fps_, want_w_, want_h_);
        }

        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        const HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) {
            if (sample) sample->Release();
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) sample->Release();
            return false;
        }
        // A stream-type change (a resolution switch mid-movie) re-reads the size instead of reallocating from a
        // stale one.
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) reader_format(reader_, w_, h_, fps_);
        if (!sample) return true;  // a gap (no sample this call): not end of stream, just nothing to emit

        std::vector<uint8_t> tight;
        const bool ok = to_nv12(sample, tight);
        sample->Release();
        if (!ok) return false;

        const uint32_t out_w = want_w_ ? want_w_ : w_;
        const uint32_t out_h = want_h_ ? want_h_ : h_;
        if (scaled_ && (w_ != out_w || h_ != out_h)) {
            rescale_nv12(tight, w_, h_, nv12, out_w, out_h);
        } else {
            nv12 = std::move(tight);
        }
        pts_us = int64_t(ts / 10);  // Media Foundation timestamps are 100 ns units; the guest path wants µs
        return true;
    }

    // Looping title/background movies seek back to the first frame. A failed seek ends the session rather than
    // spinning on a stream that will never produce another frame.
    bool rewind() override {
        if (!reader_) return false;
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = 0;
        // REFPROPVARIANT is a reference in the MinGW header and a pointer in the SDK one, so pass a dereferenced
        // value: both bind to it.
        const PROPVARIANT& ref = pos;
        HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, ref);
        PropVariantClear(&pos);
        if (FAILED(hr)) return false;
        hr = reader_->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        return SUCCEEDED(hr);
    }

    void stop() override {
        stopped_.store(true, std::memory_order_relaxed);
        if (reader_) {
            reader_->Release();
            reader_ = nullptr;
        }
    }

    uint32_t width() const override { return want_w_ ? want_w_ : w_; }
    uint32_t height() const override { return want_h_ ? want_h_ : h_; }

  private:
    // Copy one decoded NV12 sample into a tightly packed buffer (w*h luma + w*h/2 chroma). The locked buffer's
    // stride is derived from its length, because the video processor pads rows to its own alignment.
    bool to_nv12(IMFSample* sample, std::vector<uint8_t>& out) {
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length)) || !data || length == 0) {
            buffer->Release();
            return false;
        }
        const uint32_t w = w_, h = h_;
        const size_t tight = size_t(w) * h * 3 / 2;
        if (length < tight) {  // smaller than the frame it claims to be: nothing safe to copy
            buffer->Unlock();
            buffer->Release();
            return false;
        }
        const uint32_t stride = uint32_t(length / (size_t(h) * 3 / 2));
        if (stride == w || length == tight) {
            out.assign(data, data + tight);
        } else {
            out.resize(tight);
            for (uint32_t y = 0; y < h; y++)
                std::memcpy(out.data() + size_t(y) * w, data + size_t(y) * stride, w);
            for (uint32_t y = 0; y < h / 2; y++)
                std::memcpy(out.data() + size_t(w) * h + size_t(y) * w, data + size_t(y + h) * stride, w);
        }
        buffer->Unlock();
        buffer->Release();
        return true;
    }

    std::string host_;
    uint32_t want_w_ = 0, want_h_ = 0;
    IMFSourceReader* reader_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    double fps_ = 30.0;
    bool scaled_ = false;
    std::atomic<bool> stopped_{false};
};

}  // namespace

bool probe(const std::string& host_path, uint32_t& w, uint32_t& h, double& fps) {
    IMFSourceReader* reader = open_reader(host_path, 0, 0);
    if (!reader) return false;
    const bool ok = reader_format(reader, w, h, fps);
    if (ok) fps = measure_rate(reader, fps);
    reader->Release();
    return ok;
}

std::unique_ptr<Stream> open(const std::string& host_path, uint32_t want_w, uint32_t want_h) {
    return std::make_unique<MfStream>(host_path, want_w, want_h);
}

}  // namespace nx::movie_win
