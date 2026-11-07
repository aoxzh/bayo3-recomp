// Focused tests for the production dual-pipe reader. No game or device is used.
#include "../runtime/src/movie_decoder_pipe.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using nx::movie_decoder_pipe::Reader;
using nx::movie_decoder_pipe::Result;

struct Pipes {
    int pixels[2] = {-1, -1};
    int metadata[2] = {-1, -1};
    Pipes() { assert(::pipe(pixels) == 0 && ::pipe(metadata) == 0); }
    ~Pipes() {
        for (int fd : pixels) if (fd >= 0) ::close(fd);
        for (int fd : metadata) if (fd >= 0) ::close(fd);
    }
    void close_writers() {
        ::close(pixels[1]); pixels[1] = -1;
        ::close(metadata[1]); metadata[1] = -1;
    }
    void writers_closed_by_thread() { pixels[1] = -1; metadata[1] = -1; }
};

static bool write_all(int fd, const void* data, size_t bytes) {
    auto* p = static_cast<const char*>(data);
    while (bytes) {
        const ssize_t n = ::write(fd, p, bytes);
        if (n > 0) {
            p += n;
            bytes -= size_t(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}
static bool write_fragments(int fd, const void* data, size_t bytes, size_t step) {
    auto* p = static_cast<const char*>(data);
    for (size_t at = 0; at < bytes;) {
        const size_t n = std::min(step, bytes - at);
        if (!write_all(fd, p + at, n)) return false;
        at += n;
    }
    return true;
}
static std::string stats(unsigned n, const char* pts, const char* tb, size_t size) {
    return "NXPTS " + std::to_string(n) + " " + pts + " " + tb + " " + std::to_string(size) + "\n";
}

static void fragmented(bool pixels_first) {
    constexpr size_t size = 100003; // larger than pipe capacity; raw reads must make progress
    Pipes p;
    const int out = p.pixels[1], meta = p.metadata[1];
    std::thread writer([=] {
        const char* times[] = {"-1", "125", "987"};
        for (unsigned i = 0; i < 3; ++i) {
            const auto record = stats(i, times[i], "1/1000", size);
            std::vector<uint8_t> payload(size, uint8_t(0x31 + i));
            bool ok = true;
            if (pixels_first) {
                ok = write_fragments(out, payload.data(), payload.size(), 997);
                ok = ok && write_fragments(meta, record.data(), record.size(), 2);
            } else {
                ok = write_fragments(meta, record.data(), record.size(), 2);
                ok = ok && write_fragments(out, payload.data(), payload.size(), 997);
            }
            if (!ok) break;
        }
        ::close(out);
        ::close(meta);
    });
    Reader reader(p.pixels[0], p.metadata[0], size);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    const int64_t expected[] = {-1000, 125000, 987000};
    for (unsigned i = 0; i < 3; ++i) {
        assert(reader.next(frame, us) == Result::frame && us == expected[i]);
        assert(frame.size() == size && std::all_of(frame.begin(), frame.end(),
                                                   [i](uint8_t v) { return v == 0x31 + i; }));
    }
    assert(reader.next(frame, us) == Result::eof);
    assert(reader.next(frame, us) == Result::eof);
    writer.join();
    p.writers_closed_by_thread();
}

static void long_noise() {
    Pipes p;
    const int out = p.pixels[1], meta = p.metadata[1];
    std::thread writer([=] {
        // Fill the metadata pipe many times before any pixel can be written.
        std::string noise(140000, 'x');
        noise.replace(6000, 5, "NXPTS"); // suffix of an oversized log is not a stats line
        noise += "\n";
        assert(write_all(meta, noise.data(), noise.size()));
        const std::string line = stats(0, "17", "1/1000", 4);
        assert(write_all(meta, line.data(), line.size()));
        assert(write_all(out, "ABCD", 4));
        ::close(out);
        ::close(meta);
    });
    Reader reader(p.pixels[0], p.metadata[0], 4);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    assert(reader.next(frame, us) == Result::frame && us == 17000);
    assert(std::string(frame.begin(), frame.end()) == "ABCD");
    assert(reader.next(frame, us) == Result::eof);
    writer.join();
    p.writers_closed_by_thread();
}

static void rounding_and_range() {
    struct Example { const char* pts; const char* tb; int64_t expected; };
    const Example cases[] = {
        {"1", "1/2000000", 1}, {"-1", "1/2000000", -1},
        {"3", "1/2000000", 2}, {"-3", "1/2000000", -2},
        {"1", "1/3", 333333}, {"2", "1/3", 666667},
        {"9223372036854775807", "1/1000000", std::numeric_limits<int64_t>::max()},
        {"-9223372036854775808", "1/1000000", std::numeric_limits<int64_t>::min()},
        {"9223372036854775807", "18446744073709/18446744073709551615", INT64_C(9223372036854500000)},
    };
    Pipes p;
    for (unsigned i = 0; i < std::size(cases); ++i) {
        const auto line = stats(i, cases[i].pts, cases[i].tb, 4);
        assert(write_all(p.metadata[1], line.data(), line.size()));
        const uint8_t pixels[] = {uint8_t(i), 1, 2, 3};
        assert(write_all(p.pixels[1], pixels, sizeof pixels));
    }
    p.close_writers();
    Reader reader(p.pixels[0], p.metadata[0], 4);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    for (unsigned i = 0; i < std::size(cases); ++i) {
        assert(reader.next(frame, us) == Result::frame);
        assert(us == cases[i].expected && frame.size() == 4 && frame[0] == i);
    }
    assert(reader.next(frame, us) == Result::eof);
}

static void bad_case(const std::string& meta, const std::string& raw, size_t size = 4) {
    Pipes p;
    assert(write_all(p.metadata[1], meta.data(), meta.size()));
    assert(write_all(p.pixels[1], raw.data(), raw.size()));
    p.close_writers();
    Reader reader(p.pixels[0], p.metadata[0], size);
    std::vector<uint8_t> frame;
    int64_t us = 123;
    assert(reader.next(frame, us) == Result::error);
    assert(reader.next(frame, us) == Result::error);
}

static void malformed() {
    const std::string raw = "ABCD";
    bad_case("NXPTS 1 0 1/1000 4\n", raw); // wrong ordinal
    bad_case("NXPTS 0 0 1/1000 3\n", raw); // wrong packet size
    bad_case("NXPTS 0 0 0/1000 4\n", raw);
    bad_case("NXPTS 0 0 1/0 4\n", raw);
    bad_case("NXPTS 0 0 -1/1000 4\n", raw);
    bad_case("NXPTS 0 +1 1/1000 4\n", raw);
    bad_case("NXPTS 0 0 1/1000 4 extra\n", raw);
    bad_case("NXPTS 0 0 1/1000\n", raw);
    bad_case("NXPTS 0 0 1/1000 4", raw); // incomplete line
    bad_case("NXPTSx 0 0 1/1000 4\n", raw);
    bad_case("not a stats line\n", raw);
    bad_case("NXPTS 0 9223372036854775807 1/1 4\n", raw);
    bad_case("NXPTS 0 -9223372036854775808 1/1 4\n", raw);
    bad_case("NXPTS 0 9223372036854775807 18446744073709551615/1 4\n", raw);
    bad_case("NXPTS 0 9223372036854775808 1/1000000 4\n", raw);
    bad_case("NXPTS 0 0 1/1000 4\n", "ABC"); // truncated frame
    bad_case("NXPTS 0 0 1/1000 4\n", ""); // orphan metadata
    bad_case("NXPTS " + std::string(4097, '9') + "\n", raw);
    std::string too_many;
    for (unsigned i = 0; i < 65; ++i) too_many += stats(i, "0", "1/1000", 4);
    bad_case(too_many, ""); // bounded future stats fail rather than block

    Pipes p;
    p.close_writers();
    Reader empty(p.pixels[0], p.metadata[0], 4);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    assert(empty.next(frame, us) == Result::eof);
    Reader same_fd(p.pixels[0], p.pixels[0], 4);
    assert(same_fd.next(frame, us) == Result::error);
    Reader empty_frame(p.pixels[0], p.metadata[0], 0);
    assert(empty_frame.next(frame, us) == Result::error);

    // A valid first frame cannot hide a second frame with missing metadata.
    Pipes p2;
    const auto line = stats(0, "0", "1/1000", 4);
    assert(write_all(p2.metadata[1], line.data(), line.size()));
    assert(write_all(p2.pixels[1], "ABCDEFGH", 8));
    p2.close_writers();
    Reader missing(p2.pixels[0], p2.metadata[0], 4);
    assert(missing.next(frame, us) == Result::frame);
    assert(missing.next(frame, us) == Result::error);
}

static void many_buffered_small_frames() {
    // More than the parsed timestamp queue fits in one metadata read, and all
    // corresponding raw frames are already available. No valid frame is lost.
    Pipes p;
    std::string meta, pixels;
    for (unsigned i = 0; i < 120; ++i) {
        const auto number = std::to_string(i);
        meta += stats(i, number.c_str(), "1/1000", 4);
        pixels.append(4, char(i));
    }
    assert(meta.size() < 4096);
    assert(write_all(p.metadata[1], meta.data(), meta.size()));
    assert(write_all(p.pixels[1], pixels.data(), pixels.size()));
    p.close_writers();
    Reader reader(p.pixels[0], p.metadata[0], 4);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    for (unsigned i = 0; i < 120; ++i) {
        assert(reader.next(frame, us) == Result::frame);
        assert(us == int64_t(i) * 1000 && frame == std::vector<uint8_t>(4, uint8_t(i)));
    }
    assert(reader.next(frame, us) == Result::eof);
}

static void metadata_flood() {
    // A broken producer writes far more than any ordinary pipe capacity of
    // future records before its first pixel. It must fail rather than deadlock.
    Pipes p;
    const int out = p.pixels[1], meta = p.metadata[1];
    std::thread writer([=] {
        std::string records;
        for (unsigned i = 0; i < 100000; ++i) records += stats(i, "0", "1/1000", 4);
        if (write_all(meta, records.data(), records.size())) write_all(out, "ABCD", 4);
        ::close(out);
        ::close(meta);
    });
    Reader reader(p.pixels[0], p.metadata[0], 4);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    assert(reader.next(frame, us) == Result::error);
    ::close(p.metadata[0]); p.metadata[0] = -1;
    writer.join();
    p.writers_closed_by_thread();
}

static void borrowed_descriptors() {
    Pipes p;
    p.close_writers();
    {
        Reader reader(p.pixels[0], p.metadata[0], 4);
        std::vector<uint8_t> frame;
        int64_t us = 0;
        assert(reader.next(frame, us) == Result::eof);
    }
    assert(::fcntl(p.pixels[0], F_GETFD) != -1);
    assert(::fcntl(p.metadata[0], F_GETFD) != -1);
}

static void stopped_child() {
    Pipes p;
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::close(p.pixels[0]);
        ::close(p.metadata[0]);
        for (;;) ::pause();
    }
    p.close_writers();
    Reader reader(p.pixels[0], p.metadata[0], 4);
    std::atomic<bool> started{false};
    Result result = Result::error;
    std::thread reader_thread([&] {
        std::vector<uint8_t> frame;
        int64_t us = 0;
        started = true;
        result = reader.next(frame, us);
    });
    while (!started.load()) std::this_thread::yield();
    assert(::kill(child, SIGKILL) == 0);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    reader_thread.join();
    assert(result == Result::eof);
}

static void real_ffmpeg(const char* program) {
    Pipes p;
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        assert(::dup2(p.pixels[1], STDOUT_FILENO) >= 0);
        assert(::dup2(p.metadata[1], STDERR_FILENO) >= 0);
        ::close(p.pixels[0]); ::close(p.pixels[1]);
        ::close(p.metadata[0]); ::close(p.metadata[1]);
        ::execlp(program, program,
            "-nostdin", "-hide_banner", "-loglevel", "error", "-nostats", "-f", "lavfi", "-i",
            "nullsrc=s=16x16:r=1000:d=0.004,geq=lum='N*40+16':cb=128:cr=128",
            "-vf", "setpts='if(eq(N,0),2000,if(eq(N,1),2125,if(eq(N,2),2500,2875)))'",
            "-copyts", "-fps_mode", "passthrough", "-enc_time_base", "-1",
            "-stats_mux_pre", "pipe:2", "-stats_mux_pre_fmt", "NXPTS {n} {pts} {tb} {size}",
            "-frames:v", "4", "-c:v", "rawvideo", "-pix_fmt", "nv12", "-f", "rawvideo", "pipe:1",
            static_cast<char*>(nullptr));
        _exit(127);
    }
    p.close_writers();
    Reader reader(p.pixels[0], p.metadata[0], 384);
    std::vector<uint8_t> frame;
    int64_t us = 0;
    const int64_t times[] = {2000000, 2125000, 2500000, 2875000};
    const uint8_t luma[] = {16, 56, 96, 136};
    for (unsigned i = 0; i < 4; ++i) {
        assert(reader.next(frame, us) == Result::frame);
        assert(us == times[i] && frame.size() == 384 && frame[0] == luma[i]);
    }
    assert(reader.next(frame, us) == Result::eof);
    int status = 0;
    assert(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char** argv) {
    ::signal(SIGPIPE, SIG_IGN);
    fragmented(false);
    fragmented(true);
    long_noise();
    rounding_and_range();
    malformed();
    many_buffered_small_frames();
    metadata_flood();
    borrowed_descriptors();
    stopped_child();
    std::puts("synthetic dual-pipe regression passed");
    if (argc == 2) {
        real_ffmpeg(argv[1]);
        std::puts("real FFmpeg variable/nonzero PTS and NV12 frame order passed");
    }
}
