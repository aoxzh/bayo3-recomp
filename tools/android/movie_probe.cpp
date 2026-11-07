// Standalone check for the Android hardware cutscene decoder (runtime/src/movie_android.cpp).
//
// The game only opens a movie when a cutscene starts, which makes "does our MediaCodec path produce the right
// pixels" hard to answer from the game log alone. This program drives the same Stream interface directly against a
// romfs .webm and writes the decoded frames as PPM, so the pixels can be inspected without navigating the title.
//
// Build (arm64, API 29, same as the APK):
//   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++ \
//       -std=c++20 -O2 -Iruntime/include -Iruntime/src tools/android/movie_probe.cpp \
//       runtime/src/movie_android.cpp -landroid -llog -lmediandk -o /tmp/movie_probe
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "movie_android.h"

namespace nx {
// The runtime's logger is not linked here; movie_android.cpp only needs it to exist.
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
}  // namespace nx

// NV12 -> RGB, integer only. Close enough to judge whether the picture is right.
static void write_ppm(const char* path, const std::vector<uint8_t>& nv12, uint32_t w, uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    const uint8_t* yp = nv12.data();
    const uint8_t* uv = nv12.data() + size_t(w) * h;
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const int Y = yp[size_t(y) * w + x];
            const int U = uv[size_t(y / 2) * w + (x / 2) * 2] - 128;
            const int V = uv[size_t(y / 2) * w + (x / 2) * 2 + 1] - 128;
            int r = Y + ((1436 * V) >> 10);
            int g = Y - ((352 * U + 731 * V) >> 10);
            int b = Y + ((1815 * U) >> 10);
            row[x * 3 + 0] = uint8_t(r < 0 ? 0 : r > 255 ? 255 : r);
            row[x * 3 + 1] = uint8_t(g < 0 ? 0 : g > 255 ? 255 : g);
            row[x * 3 + 2] = uint8_t(b < 0 ? 0 : b > 255 ? 255 : b);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <movie.webm> [frames] [outprefix] [--loop]\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    const int want = argc > 2 ? std::atoi(argv[2]) : 3;
    const std::string prefix = argc > 3 ? argv[3] : "/data/local/tmp/frame";
    // --loop exercises Stream::rewind(), the path the looping title/background movies use. Nothing else in the
    // game calls it early enough to observe, so without this the seek-back would go to the phone untested.
    bool loop_once = argc > 4 && std::strcmp(argv[4], "--loop") == 0;

    uint32_t pw = 0, ph = 0;
    double fps = 30.0;
    if (!nx::movie_android::probe(path, pw, ph, fps)) {
        std::fprintf(stderr, "PROBE FAILED\n");
        return 1;
    }
    std::printf("PROBE ok: %ux%u @ %.3f fps\n", pw, ph, fps);

    auto s = nx::movie_android::open(path, pw, ph);
    if (!s) {
        std::fprintf(stderr, "OPEN FAILED\n");
        return 1;
    }
    std::vector<uint8_t> nv12;
    int got = 0;
    for (int i = 0; i < want; i++) {
        if (!s->next(nv12)) {
            std::fprintf(stderr, "NEXT FAILED at frame %d\n", i);
            break;
        }
        // A frame with no variance is a frame of one flat colour, which is what a broken layout looks like.
        uint64_t sum = 0, sum2 = 0;
        for (size_t k = 0; k < size_t(pw) * ph; k++) {
            sum += nv12[k];
            sum2 += uint64_t(nv12[k]) * nv12[k];
        }
        const double n = double(size_t(pw) * ph);
        const double mean = double(sum) / n;
        const double var = double(sum2) / n - mean * mean;
        char out[256];
        std::snprintf(out, sizeof out, "%s-%02d.ppm", prefix.c_str(), i);
        write_ppm(out, nv12, pw, ph);
        std::printf("frame %d: %zu bytes, luma mean %.1f var %.1f -> %s\n", i, nv12.size(), mean, var, out);
        got++;
    }
    if (loop_once) {
        if (!s->rewind()) {
            std::fprintf(stderr, "REWIND FAILED\n");
            s->stop();
            return 1;
        }
        std::vector<uint8_t> again;
        int back = 0;
        while (back < want && s->next(again)) back++;
        std::printf("REWIND ok: %d more frames after seeking to 0\n", back);
        got = back == want ? got : 0;
    }
    s->stop();
    std::printf("DONE: %d frames decoded\n", got);
    return got == want ? 0 : 1;
}
