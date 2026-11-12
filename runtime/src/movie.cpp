// Software movie decode behind the faked nvdec.
//
// The game plays its cutscenes through NvMMLite on /dev/nvhost-nvdec; this runtime completes every
// ioctl and fence immediately without any hardware behind them, so the decoder output stays the
// blank YUV the game initialised (luma 0, chroma 127) and the picture never exists. What does work
// end to end is everything downstream: WriteTexels copies the planes into textures, the YUV
// composite pass renders them, audio plays in real time. So the decode is the only missing piece,
// and it is supplied here: ffmpeg decodes the same .webm on the host into NV12 — the exact R8 luma
// + RG8 interleaved-chroma pair the game's planes are — and the frames are written straight into
// the guest buffers those WriteTexels calls copy from.
//
// The verified main-module fetch supplies the guest-selected presentation PTS.
// FFmpeg's per-packet timestamp stream keeps that selection independent of
// average frame rate, upload cadence, or when the first decoded frame arrives.
// Paths without that binding retain the existing wall-clock fallback.
#include "movie.h"
#include "movie_timing.h"

// The host decoder is an ffmpeg subprocess: two pipes, poll() and a fork()ed child. That plumbing is POSIX-only, so
// every platform without it brings its own decoder instead: Android uses MediaCodec, Windows uses Media Foundation
// with the VP9 transform that ships with Windows 10/11. All three sit behind the same "decode one movie into NV12
// planes" job, so everything downstream of the decoder — the Y/UV plane fill, the guest presentation-time
// selection, the composite pass — is platform-neutral and unchanged.
#if !defined(__ANDROID__) && !defined(_WIN32)
#define NX_MOVIE_HOST_DECODER 1
#include "movie_decoder_pipe.h"
#endif

#if defined(NX_MOVIE_HOST_DECODER)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

#include <chrono>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <nx/runtime.h>

#if defined(__ANDROID__)
#include "movie_android.h"
#elif defined(_WIN32)
#include "movie_win.h"
#endif

#if defined(NX_MOVIE_HOST_DECODER)
// posix_spawnp() reads the parent's environment through this.
extern char** environ;
#endif

namespace nx {

#if defined(NX_MOVIE_VTABLE)
// runtime/src/hle_movie_api.cpp (compiled only when that option is on): the late player-vtable patch and the
// cumulative per-slot call profile.
namespace plat {
void movie_vtable_late_player();
void movie_vtable_counts(const char* why);
void movie_vtable_new_session();
}  // namespace plat
#endif

namespace {

constexpr uint32_t kFmtR8 = 0x1;   // luma plane
constexpr uint32_t kFmtRG8 = 0xd;  // interleaved chroma plane (NV12's second half, byte for byte)

bool movie_feed_disabled() {
    static const bool off = std::getenv("NX_NO_MOVIE_FEED") != nullptr;
    return off;
}

bool likely_looping_background(const std::string& guest) {
    std::string p = guest;
    for (char& c : p) c = char(std::tolower(static_cast<unsigned char>(c)));
    return p.find("title") != std::string::npos || p.find("logo") != std::string::npos ||
           p.find("loop") != std::string::npos || p.find("background") != std::string::npos;
}

constexpr int64_t kNoPts = std::numeric_limits<int64_t>::min();

struct Frame {
    uint64_t index = 0;              // position in the decoded stream
    std::vector<uint8_t> nv12;       // w*h luma then w*h/2 interleaved chroma
    int64_t pts_us = kNoPts;         // source presentation time, never inferred from average FPS
};

// Fetch and its plane uploads run synchronously on the same guest renderer
// thread. Each attempt clears this record, including failed/rejected fetches.
struct PendingFrame {
    uint64_t file_handle = 0;
    int64_t pts_us = 0;
    uint64_t luma = 0, chroma = 0;
    bool luma_filled = false;
};
thread_local PendingFrame g_pending_frame;

struct Session {
    std::string guest, host;
    uint64_t file_handle = 0;
    uint32_t src_w = 0, src_h = 0;  // as encoded
    uint32_t out_w = 0, out_h = 0;  // as the game's planes want it (scaled on decode if needed)
    double fps = 30.0;
#if defined(NX_MOVIE_HOST_DECODER)
    pid_t ff = 0;
    int fd = -1;                     // ffmpeg stdout
    int metadata_fd = -1;            // ffmpeg packet timestamps / stderr
#elif defined(__ANDROID__)
    // Android has no ffmpeg to fork: the phone's own hardware decoder plays the same role, one frame at a time.
    std::unique_ptr<movie_android::Stream> android;
#elif defined(_WIN32)
    // Windows has no ffmpeg on PATH either: Media Foundation decodes the same WebM/VP9 movies through the VP9
    // transform that ships with the OS.
    std::unique_ptr<movie_win::Stream> win;
#endif
    std::thread reader;
    std::mutex m;
    std::mutex stop_m;  // serialize join/reap if stop is called more than once
#if defined(__ANDROID__) || defined(_WIN32)
    std::mutex android_rewind_m;  // do not race decoder rewind with stop
#endif
    std::condition_variable space;
    std::deque<Frame> ring;          // pending decoded frames, oldest first; at most 8 plus the current latch
    uint64_t next_index = 0;
    bool eof = false, stopping = false, loop = false;
    std::vector<uint8_t> current;    // frame being served to the game (kept for re-uploads)
    uint64_t current_index = 0;
    int64_t current_pts_us = kNoPts;
    bool current_set = false;
    bool guest_timing = false, rewind_requested = false;
    int64_t target_pts_us = 0;
    bool t0_set = false;
    std::chrono::steady_clock::time_point t0;
    // How many plane writes this movie actually served. The acceptance evidence for a cutscene is exactly this
    // counter: `ev0000` reaches 4 digits per plane, `ev0005` currently stays at 0 (docs/archive/MOVIE-EV0005.md).
    std::atomic<uint64_t> fills_luma{0}, fills_chroma{0};
    // Serializes this film's own plane fills. A fill copies out of `current` without holding the session lock and may
    // also stop and restart this film's decoder; fills of different films do not wait for each other.
    std::mutex fill_m;

    size_t frame_bytes() const { return size_t(out_w) * out_h * 3 / 2; }

#if defined(NX_MOVIE_HOST_DECODER)
    // A detached PID cannot be signalled by stop after it has been reaped and
    // potentially reused. Kill while holding m, detach, then close/reap outside.
    void release_child() {
        int old_fd, old_metadata_fd;
        pid_t old_ff;
        {
            std::lock_guard lk(m);
            old_fd = fd;
            old_metadata_fd = metadata_fd;
            old_ff = ff;
            fd = metadata_fd = -1;
            ff = 0;
            if (old_ff) kill(old_ff, SIGKILL);
        }
        if (old_fd >= 0) close(old_fd);
        if (old_metadata_fd >= 0) close(old_metadata_fd);
        if (old_ff) while (waitpid(old_ff, nullptr, 0) < 0 && errno == EINTR) {}
    }
#endif

    void stop() {
        std::lock_guard stop_lk(stop_m);
        {
            std::lock_guard lk(m);
            if (stopping) return;
            stopping = true;
#if defined(NX_MOVIE_HOST_DECODER)
            if (ff) kill(ff, SIGKILL);  // wake a reader blocked in read()
#endif
        }
        space.notify_all();
#if defined(__ANDROID__)
        {
            std::lock_guard lk(android_rewind_m);
            if (android) android->stop();  // wake a reader blocked in the decoder
        }
#elif defined(_WIN32)
        {
            std::lock_guard lk(android_rewind_m);
            if (win) win->stop();  // the reader returns at its next frame, then the join below completes
        }
#endif
        if (reader.joinable()) reader.join();
#if defined(NX_MOVIE_HOST_DECODER)
        release_child();
#endif
#if defined(__ANDROID__)
        android.reset();
#elif defined(_WIN32)
        win.reset();
#endif
    }

    ~Session() { stop(); }
};

// With a verified guest target, neither producer nor consumer advances during
// a guest pause. A late producer discards obsolete frames while catching up.
// Other paths retain their prior wall-clock pacing and bounded preroll.
bool prune_frames(Session& s) {  // s.m held
    if (s.guest_timing) return movie_timing::prune_pts(s.ring, s.target_pts_us);
    const auto now = movie_timing::Clock::now();
    return s.t0_set && now >= s.t0 &&
           movie_timing::prune_due(s.ring, movie_timing::target(s.t0, now, s.fps));
}

bool enqueue_frame(Session& s, std::vector<uint8_t> nv12, int64_t pts_us = kNoPts) {
    std::unique_lock lk(s.m);
    for (;;) {
        if (s.stopping) return false;
        prune_frames(s);
        if (s.ring.size() < 8) break;
        if (s.guest_timing || !s.t0_set) {
            s.space.wait(lk);
        } else {
            s.space.wait_until(lk, movie_timing::deadline(s.t0, s.ring[1].index, s.fps));
        }
    }
    s.ring.push_back(Frame{s.next_index++, std::move(nv12), pts_us});
    prune_frames(s);
    return true;
}

// Every open movie gets its own session for as long as the game holds it open. The game pre-opens the next cutscene
// while the current one is still playing — in the prologue's ev0000 -> ev0005 -> ev0010 -> ev0011 chain it opens
// ev0011 about two uploaded frames after ev0010's first one (handle 721 then 737, log line 3145) — so "one session" is
// not a property of the player, it is a property of the file handle. A session lives exactly as long as its handle.
std::mutex g_movie;
// shared_ptr, not unique_ptr: a plane fill keeps its session alive while it copies, without holding the container
// lock (a close or an eviction may remove it from the container meanwhile).
std::deque<std::shared_ptr<Session>> g_sessions;
// The chain peaks at two live sessions (the film playing and the one the game pre-opened); four leaves room for a
// third and still bounds a game that opens without closing (docs/archive/MOVIE-EV0005.md has seen an ev0020 that was
// never closed before ev0021 opened).
constexpr size_t kMaxSessions = 4;
// The film that served the last luma plane. Two uses: the next chroma write of that same pair must come from the same
// film (a Y/UV pair from two different cutscenes is a torn picture), and movie_plane_fallback names it when a write had
// no frame. Atomic because a fill publishes it without taking the container lock.
std::atomic<uint64_t> g_picture_handle{0};

std::shared_ptr<Session> find_session(uint64_t file_handle) {
    for (auto& s : g_sessions)
        if (s->file_handle == file_handle) return s;
    return nullptr;
}

// Does this session's own decoded size produce the plane an upload of (tex_w x tex_h) wants?
bool shape_matches(const Session& s, bool luma, uint32_t tex_w, uint32_t tex_h) {
    if (!s.out_w || !s.out_h) return false;
    return tex_w == (luma ? s.out_w : s.out_w / 2) && tex_h == (luma ? s.out_h : s.out_h / 2);
}

// Which films may answer one plane write, best first. Ordered, not filtered: the caller offers the write to each in
// turn and the first film that can actually produce the plane wins, which is what keeps a handover from going black.
//
// The order is: for a chroma write, the film that served this pair's luma plane (see g_picture_handle) — a Y/UV pair
// from two different cutscenes is a torn picture; then the film the renderer just fetched a verified frame from (a
// guest-PTS build names the playing film exactly); then the OLDEST open film that has not played out, and within that a
// real film whose own size already matches the write ahead of one that would have to be rescaled, and both ahead of a
// looping background.
//
// Oldest first is what open order demands. The game pre-opens the next cutscene while the current one still plays
// (ev0011 opens two uploaded frames after ev0010's first) and keeps playing the current one's sound, so the film that
// is merely pre-opened is the one waiting its turn. Issue #3 is what newest-first does on a build without a verified
// fetch (Android): ev0011's picture took over one frame into ev0010, under ev0010's sound, and ev0011 then ended early
// leaving its own sound on black. The pre-opened film's clock does not start until it serves its first plane, so it
// starts from its own first frame when the older film is closed or has played out (decoded to the end and shown its
// last frame). A luma write does not prefer g_picture_handle for the same reason: the older film must hand over.
// Size before age because a lingering 896x496 weapon or scene film must never be stretched onto a 1600x900 cutscene
// plane — and must never be the reason the playing film's decoder is restarted. Loops last because a looping
// title/background never reaches EOF and would otherwise hold the picture on open order alone.
std::vector<std::shared_ptr<Session>> plane_candidates(bool luma, uint32_t tex_w, uint32_t tex_h) {
    std::vector<std::shared_ptr<Session>> out;
    std::lock_guard lk(g_movie);
    const auto add = [&out](const std::shared_ptr<Session>& s) {
        for (const auto& have : out)
            if (have.get() == s.get()) return;
        out.push_back(s);
    };
    if (!luma && g_picture_handle)
        if (auto p = find_session(g_picture_handle); p && !p->stopping) add(p);
    if (g_pending_frame.file_handle)
        if (auto p = find_session(g_pending_frame.file_handle); p && !p->stopping) add(p);
    // A finished film stays a candidate: its last frame is the picture the game keeps uploading after the cutscene
    // ends, so dropping it would black out the screen between films. It ranks last, and only a luma write may restart
    // it (see movie_fill), because restarting a film to answer a chroma write would serve a frame of the wrong pair.
    // One consistent snapshot per film (g_movie, then each film's own lock: nothing takes them in the other order).
    std::vector<int> ranks(g_sessions.size(), -1);
    for (size_t i = 0; i < g_sessions.size(); i++) {
        Session& s = *g_sessions[i];
        std::lock_guard slk(s.m);
        if (s.stopping) continue;
        // Played out = decoded to the end AND every decoded frame already shown. Decoder EOF alone runs up to eight
        // frames ahead of the picture, and handing over then would cut the older film's last quarter second.
        const bool played_out = s.eof && s.ring.empty();
        ranks[i] = (played_out ? 4 : 0) + (shape_matches(s, luma, tex_w, tex_h) ? 0 : 2) + (s.loop ? 1 : 0);
    }
    for (int rank = 0; rank < 7; rank++)
        for (size_t i = 0; i < g_sessions.size(); i++)  // oldest open first
            if (ranks[i] == rank) add(g_sessions[i]);
    return out;
}

// Takes one session out of the container and hands it to the caller, so the caller can stop it and drop the last
// reference (which joins the reader thread and closes the decoder) after releasing the container lock.
std::shared_ptr<Session> take_session(uint64_t file_handle) {
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it)
        if ((*it)->file_handle == file_handle) {
            std::shared_ptr<Session> out = std::move(*it);
            g_sessions.erase(it);
            return out;
        }
    return nullptr;
}

// One session to drop so a new open fits: a finished or already stopping one if there is any, otherwise the oldest
// that is not the picture. g_movie held.
std::shared_ptr<Session> evict_one() {
    auto pick = g_sessions.end();
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it)
        if ((*it)->stopping || (*it)->eof) {
            pick = it;
            break;
        }
    if (pick == g_sessions.end()) {
        bool kept_the_picture = false;
        for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
            const bool playable = !(*it)->stopping && !(*it)->eof && !(*it)->loop;
            if (playable && !kept_the_picture) {
                kept_the_picture = true;
                continue;
            }
            pick = it;
            break;
        }
    }
    if (pick == g_sessions.end()) {
        if (g_sessions.empty()) return nullptr;
        pick = g_sessions.begin();
    }
    std::shared_ptr<Session> out = std::move(*pick);
    g_sessions.erase(pick);
    return out;
}

// One line per movie session end. The plane-fill counts are the acceptance evidence: a working movie reaches four
// digits per plane (ev0000), a movie the game never uploads stays at exactly 0 (ev0005 today).
void log_session(const char* what, const Session& s) {
    log("movie: %s %s: %llu luma + %llu chroma plane fills", what, s.guest.c_str(),
        (unsigned long long)s.fills_luma.load(std::memory_order_relaxed),
        (unsigned long long)s.fills_chroma.load(std::memory_order_relaxed));
}

bool probe(const std::string& host, uint32_t& w, uint32_t& h, double& fps) {
#if defined(__ANDROID__)
    // No ffprobe in an APK: ask the platform's media stack for the same header fields.
    return movie_android::probe(host, w, h, fps);
#elif defined(_WIN32)
    // Same contract, same source of truth: Media Foundation opens the container and reports the decoded format.
    return movie_win::probe(host, w, h, fps);
#elif defined(NX_MOVIE_HOST_DECODER)
    char cmd[512];
    std::snprintf(cmd, sizeof cmd,
                  "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,avg_frame_rate"
                  " -of csv=p=0 '%s' 2>/dev/null",
                  host.c_str());
    FILE* p = popen(cmd, "r");
    if (!p) return false;
    char line[128] = {};
    size_t n = fread(line, 1, sizeof line - 1, p);
    pclose(p);
    unsigned pw = 0, ph = 0, num = 0, den = 1;
    if (n == 0 || std::sscanf(line, "%u,%u,%u/%u", &pw, &ph, &num, &den) < 2 || !pw || !ph) return false;
    if (!num || !den) num = 30, den = 1;
    w = pw;
    h = ph;
    fps = double(num) / den;
    return true;
#else
    // No host decoder on this platform (see the note at the top of the file): there is nothing to ask.
    (void)host;
    (void)w;
    (void)h;
    (void)fps;
    return false;
#endif
}
#if defined(NX_MOVIE_HOST_DECODER) && defined(__linux__)
// The host path of an executable from PATH, resolved in the parent: the fork() child may only use
// async-signal-safe calls before exec, and execv() (not execvp(), which may allocate while another thread holds
// the allocator) is the one that is.
std::string find_executable(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path || !*path) return {};
    const std::string dirs(path);
    size_t start = 0;
    for (;;) {
        const size_t end = dirs.find(':', start);
        std::string dir = dirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (dir.empty()) dir = ".";
        const std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}
#endif

#if defined(NX_MOVIE_HOST_DECODER)
bool spawn_ffmpeg(Session& s) {
    int fds[2], metadata[2];
    if (pipe(fds) != 0) return false;
    if (pipe(metadata) != 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    // These four descriptors belong only to this decoder; dup2 below exposes
    // its write ends as stdout/stderr and removes close-on-exec on those copies.
    for (int fd : {fds[0], fds[1], metadata[0], metadata[1]})
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    char scale[64], warg[16], harg[16];
    std::snprintf(warg, sizeof warg, "%u", s.out_w);
    std::snprintf(harg, sizeof harg, "%u", s.out_h);
    std::snprintf(scale, sizeof scale, "scale=%s:%s", warg, harg);
    char* argv[] = {const_cast<char*>("ffmpeg"),
                    const_cast<char*>("-nostdin"),
                    const_cast<char*>("-nostats"),
                    const_cast<char*>("-copyts"),
                    const_cast<char*>("-loglevel"),
                    const_cast<char*>("error"),
                    const_cast<char*>("-i"),
                    const_cast<char*>(s.host.c_str()),
                    const_cast<char*>("-map"),
                    const_cast<char*>("0:v:0"),
                    const_cast<char*>("-vf"),
                    scale,
                    const_cast<char*>("-pix_fmt"),
                    const_cast<char*>("nv12"),
                    const_cast<char*>("-fps_mode"),
                    const_cast<char*>("passthrough"),
                    const_cast<char*>("-enc_time_base"),
                    const_cast<char*>("-1"),
                    const_cast<char*>("-stats_mux_pre"),
                    const_cast<char*>("pipe:2"),
                    const_cast<char*>("-stats_mux_pre_fmt"),
                    const_cast<char*>("NXPTS {n} {pts} {tb} {size}"),
                    const_cast<char*>("-f"),
                    const_cast<char*>("rawvideo"),
                    const_cast<char*>("-"),
                    nullptr};
#if defined(__linux__)
    // fork/exec instead of posix_spawn, because the decode child must die with this process. Two real leaks on the
    // Deck motivated it: (1) a run that was SIGKILLed left ffmpeg alive, and (2) the child inherited the pipe's READ
    // end (posix_spawn only redirected the write end), so the parent's death never produced EPIPE and the orphan
    // sat blocked writing rawvideo forever — the Lead had to kill 5 of them. PR_SET_PDEATHSIG makes the child die
    // with us even on SIGKILL, and the read end is closed in the child so a normal parent exit also ends it.
    // Session::stop() keeps kill()+waitpid() for the normal close path; this is only about not surviving it.
    const std::string exe = find_executable("ffmpeg");
    if (exe.empty()) {
        close(fds[0]);
        close(fds[1]);
        close(metadata[0]);
        close(metadata[1]);
        log("movie: ffmpeg is not on PATH; playback stays blank");
        return false;
    }
    const pid_t child = fork();
    if (child == 0) {
        // Between fork() and execv() only async-signal-safe calls: prctl, open/dup2/close, execv.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() == 1) _exit(127);  // the runtime died between fork() and prctl()
        const int devnull_in = open("/dev/null", O_RDONLY);
        if (devnull_in > 0) { dup2(devnull_in, 0); close(devnull_in); }
        if (metadata[1] != 2) { dup2(metadata[1], 2); close(metadata[1]); }
        if (fds[1] != 1) { dup2(fds[1], 1); close(fds[1]); }
        if (fds[0] != 0) close(fds[0]);  // never hold the read end: that is what hid the parent's death
        close(metadata[0]);
        execv(exe.c_str(), argv);
        _exit(127);
    }
    close(fds[1]);
    close(metadata[1]);
    if (child < 0) {
        close(fds[0]);
        close(metadata[0]);
        return false;
    }
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, metadata[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);  // same leak fix where there is no PR_SET_PDEATHSIG
    posix_spawn_file_actions_addclose(&fa, metadata[0]);
    pid_t child = 0;
    int rc = posix_spawnp(&child, "ffmpeg", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    close(metadata[1]);
    if (rc != 0) {
        close(fds[0]);
        close(metadata[0]);
        return false;
    }
#endif
    {
        std::lock_guard lk(s.m);
        if (!s.stopping) {
            s.ff = child;
            s.fd = fds[0];
            s.metadata_fd = metadata[0];
            return true;
        }
    }
    // stop raced creation before publication. This child was never visible to
    // stop, so its creator must cancel and reap it before the reader can exit.
    kill(child, SIGKILL);
    close(fds[0]);
    close(metadata[0]);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    return false;
}

void read_frames(Session* s) {
    const size_t want = s->frame_bytes();
    if (!want) {
        std::lock_guard lk(s->m);
        s->eof = true;
        return;
    }
    for (;;) {
        int input, metadata;
        {
            std::lock_guard lk(s->m);
            if (s->stopping) return;
            input = s->fd;
            metadata = s->metadata_fd;
        }
        movie_decoder_pipe::Reader reader(input, metadata, want);
        uint64_t frames_this_pass = 0;
        std::vector<uint8_t> buf;
        movie_decoder_pipe::Result result;
        for (;;) {
            int64_t pts_us = 0;
            result = reader.next(buf, pts_us);
            if (result != movie_decoder_pipe::Result::frame) break;
            if (!enqueue_frame(*s, std::move(buf), pts_us)) return;
            ++frames_this_pass;
        }
        bool again;
        {
            std::lock_guard lk(s->m);
            if (s->stopping) return;
            s->eof = true;
            again = result == movie_decoder_pipe::Result::eof && s->loop && frames_this_pass != 0;
        }
        if (result == movie_decoder_pipe::Result::error)
            log("movie: %s decoder frame/timestamp stream ended inconsistently", s->guest.c_str());
        s->release_child();
        if (!again || !spawn_ffmpeg(*s)) return;
        {
            std::lock_guard lk(s->m);
            if (s->stopping) return;
            s->eof = false;
        }
    }
}

#endif  // NX_MOVIE_HOST_DECODER

// The reader thread's body for the two in-process decoders (Android MediaCodec, Windows Media Foundation): pull
// decoded frames one at a time, rewind at the end for the looping title/background movies, and push them into the
// same ring the ffmpeg reader fills so the rest of movie.cpp is unchanged. The two decoders differ only in whether
// they carry the source's own presentation time (Media Foundation does; MediaCodec's ByteBuffer mode does not), so
// that one difference is the lambda the caller passes and everything else is shared.
#if defined(__ANDROID__) || defined(_WIN32)
template <typename Next, typename Rewind, typename Stop>
void read_frames_decoder(Session* s, Next next, Rewind rewind, Stop stop, std::mutex& rewind_m) {
    uint64_t frames_this_pass = 0;
    while (true) {
        {
            std::lock_guard lk(s->m);
            if (s->stopping) return;
        }
        std::vector<uint8_t> nv12;
        int64_t pts_us = kNoPts;
        if (!next(nv12, pts_us)) {
            bool again = false;
            {
                std::lock_guard lk(s->m);
                if (s->stopping) return;
                s->eof = true;
                again = s->loop && frames_this_pass != 0;
            }
            if (!again) return;
            {
                std::lock_guard rewind_lk(rewind_m);
                {
                    std::lock_guard lk(s->m);
                    if (s->stopping) return;
                }
                if (!rewind()) return;
            }
            {
                std::lock_guard lk(s->m);
                if (s->stopping) return;
                s->eof = false;
            }
            frames_this_pass = 0;
            continue;
        }
        if (!enqueue_frame(*s, std::move(nv12), pts_us)) return;
        ++frames_this_pass;
    }
}

#if defined(__ANDROID__)
void read_frames_android(Session* s) {
    read_frames_decoder(
        s, [s](std::vector<uint8_t>& nv12, int64_t&) { return s->android->next(nv12); },
        [s] { return s->android->rewind(); }, [s] { s->android->stop(); }, s->android_rewind_m);
}
#endif

#if defined(_WIN32)
void read_frames_win(Session* s) {
    read_frames_decoder(
        s, [s](std::vector<uint8_t>& nv12, int64_t& pts) { return s->win->next(nv12, pts); },
        [s] { return s->win->rewind(); }, [s] { s->win->stop(); }, s->android_rewind_m);
}
#endif
#endif  // __ANDROID__ || _WIN32

}  // namespace

// Start producing frames for `s` at its current out_w/out_h. The two platforms differ only here and in the reader
// thread body, so open, respawn, and close all stay single-source.
bool start_decoder(Session& s) {
#if defined(__ANDROID__)
    s.android = movie_android::open(s.host, s.out_w, s.out_h);
    if (!s.android) return false;
    s.reader = std::thread(read_frames_android, &s);
    log("movie: %s -> Android decoder %ux%u @ %.3f fps (%s)", s.guest.c_str(), s.src_w, s.src_h, s.fps,
        s.host.c_str());
    return true;
#elif defined(_WIN32)
    s.win = movie_win::open(s.host, s.out_w, s.out_h);
    if (!s.win) return false;
    s.reader = std::thread(read_frames_win, &s);
    log("movie: %s -> Media Foundation %ux%u @ %.3f fps (%s)", s.guest.c_str(), s.out_w, s.out_h, s.fps,
        s.host.c_str());
    return true;
#elif defined(NX_MOVIE_HOST_DECODER)
    if (!spawn_ffmpeg(s)) return false;
    log("movie: %s -> ffmpeg %ux%u @ %.3f fps (%s)", s.guest.c_str(), s.src_w, s.src_h, s.fps, s.host.c_str());
    s.reader = std::thread(read_frames, &s);
    return true;
#else
    (void)s;
    return false;
#endif
}

void movie_on_open(const std::string& guest_path, const std::string& host_path, uint64_t file_handle) {
    if (movie_feed_disabled()) return;
#if defined(NX_MOVIE_VTABLE)
    plat::movie_vtable_counts("movie open");  // the previous session's profile, before the player (re)starts
    plat::movie_vtable_late_player();         // NX_MOVIE_VTABLE_PLAYER=late: patch on the first cutscene open
    plat::movie_vtable_new_session();         // per-slot counts restart: the next "movie close" profile is this movie's
#endif
    // The new session is built and started outside the container lock: opening a container with Media Foundation takes
    // hundreds of milliseconds, which every other open/close/fill would otherwise wait for. A failure returns here
    // without touching the sessions already playing — that alone was half of issue #3, where opening ev0011 and then
    // failing would have left the picture with nothing at all.
    auto s = std::make_shared<Session>();
    s->guest = guest_path;
    s->host = host_path;
    s->file_handle = file_handle;
    s->loop = likely_looping_background(guest_path);
    if (!probe(host_path, s->src_w, s->src_h, s->fps)) {
#if defined(__ANDROID__)
        log("movie: %s not decodable by the platform decoder; playback stays blank", guest_path.c_str());
#elif defined(_WIN32)
        log("movie: %s not decodable by Media Foundation; playback stays blank", guest_path.c_str());
#elif defined(NX_MOVIE_HOST_DECODER)
        log("movie: %s not decodable by ffprobe; playback stays blank", guest_path.c_str());
#else
        log("movie: %s has no host decoder on this platform; playback stays blank", guest_path.c_str());
#endif
        return;
    }
    s->out_w = s->src_w;
    s->out_h = s->src_h;
    if (!start_decoder(*s)) {
        log("movie: could not start a decoder for %s; playback stays blank", guest_path.c_str());
        return;
    }
    // Destroyed outside the lock: ~Session joins the reader thread and closes the decoder.
    std::shared_ptr<Session> evicted;
    {
        std::lock_guard lk(g_movie);
        if (g_sessions.size() >= kMaxSessions) {
            evicted = evict_one();
            if (evicted) log_session("evicting", *evicted);
        }
        g_sessions.push_back(std::move(s));
        log("movie: %s opened (handle %llu); %zu open session(s)%s%s", guest_path.c_str(),
            (unsigned long long)file_handle, g_sessions.size(),
            g_sessions.size() > 1 ? " - the game pre-opens the next cutscene" : "",
            likely_looping_background(guest_path) ? " (looping)" : "");
    }
}

void movie_on_close(const std::string& guest_path, uint64_t file_handle) {
    // The file handle, not the path, names the session: the game closes the film it opened last first (the prologue
    // closes ev0011 before ev0010), and a close for an open this code never saw must be a no-op rather than a guess.
    std::shared_ptr<Session> closed;
    {
        std::lock_guard lk(g_movie);
        closed = take_session(file_handle);
        if (!closed) return;
        if (closed->guest != guest_path)
            log("movie: closing %s, but handle %llu belongs to %s", guest_path.c_str(),
                (unsigned long long)file_handle, closed->guest.c_str());
        log("movie: %s closed; ending its playback session", closed->guest.c_str());
        log_session("stopping", *closed);
        if (g_sessions.empty()) g_picture_handle = 0;
#if defined(NX_MOVIE_VTABLE)
        plat::movie_vtable_counts("movie close");
#endif
    }
    // `closed` goes out of scope here, outside the lock.
}

static void copy_plane(const Session& s, bool luma, uint32_t tex_w, uint32_t tex_h, uint8_t* data, uint64_t row_stride) {
    const size_t row_bytes = luma ? tex_w : size_t(tex_w) * 2;  // a chroma row is width/2 RG pairs
    const uint8_t* from = s.current.data() + (luma ? 0 : size_t(s.out_w) * s.out_h);
    for (uint32_t r = 0; r < tex_h; r++) std::memcpy(data + r * row_stride, from + size_t(r) * s.out_w, row_bytes);
}

MoviePlane movie_plane_fallback(uint32_t format, uint32_t tex_w, uint32_t tex_h, uint8_t* data, uint64_t row_stride) {
    if (format != kFmtR8 && format != kFmtRG8) return MoviePlane::NotOurs;
    const bool luma = format == kFmtR8;
    if (row_stride < (luma ? uint64_t(tex_w) : uint64_t(tex_w) * 2)) return MoviePlane::Nothing;
    // A film the renderer paces by its own verified fetches: its guest buffer must never be uploaded blank.
    bool ours = false;
    for (const auto& s : plane_candidates(luma, tex_w, tex_h)) {
        if (!s->guest_timing || s->stopping || !shape_matches(*s, luma, tex_w, tex_h)) continue;
        ours = true;
        const std::shared_ptr<Session> keep = s;  // alive through the copy even if the game closes it meanwhile
        std::lock_guard flk(keep->fill_m);
        if (keep->current.size() < size_t(keep->out_w) * keep->out_h * 3 / 2) continue;
        copy_plane(*keep, luma, tex_w, tex_h, data, row_stride);
        return MoviePlane::Repeated;
    }
    return ours ? MoviePlane::Nothing : MoviePlane::NotOurs;
}

void movie_on_frame(uint64_t file_handle, int64_t pts_us, uint64_t luma, uint64_t chroma) {
    g_pending_frame = {};
#if !defined(__ANDROID__)
    // NX_MOVIE_WALLCLOCK=1: ignore the verified fetch and pace every film by wall clock and open order, as Android
    // does. A desktop run can then exercise the Android picture-selection path (issue #3 was Android-only).
    static const bool wall_clock = std::getenv("NX_MOVIE_WALLCLOCK") && std::getenv("NX_MOVIE_WALLCLOCK")[0] == '1';
    if (wall_clock) return;
    if (!file_handle || pts_us < 0 || !luma || !chroma || luma == chroma) return;
    std::shared_ptr<Session> s;
    {
        std::lock_guard lk(g_movie);
        s = find_session(file_handle);
        // Looping title/background playback and Android retain their existing
        // pacing until their decoder paths expose a corresponding source timeline.
        // A fetch names the film being played, and with several films open that name is the only exact one: the
        // film left behind at a handover keeps its own pacing and plays out its length instead of freezing.
        if (!s || s->loop) return;
    }
    std::lock_guard slk(s->m);
    if (s->stopping) return;
    if (s->guest_timing && pts_us < s->target_pts_us) s->rewind_requested = true;
    s->guest_timing = true;
    s->target_pts_us = pts_us;
    g_pending_frame = {file_handle, pts_us, luma, chroma, false};
    s->space.notify_one();
#else
    (void)file_handle; (void)pts_us; (void)luma; (void)chroma;
#endif
}

// Does the verified fetch in flight name this film and this very plane buffer? Only a verified fetch attributes a
// write to one film exactly; without one (wall-clock pacing) every open film may answer.
bool fetch_owns(const Session& s, bool luma, const uint8_t* data) {
    if (!s.guest_timing) return true;
    const uint64_t h = g_pending_frame.file_handle;
    if (!h || h != s.file_handle) return false;  // no fetch, or a fetch for another film
    if (reinterpret_cast<uintptr_t>(data) != (luma ? g_pending_frame.luma : g_pending_frame.chroma)) return false;
    return luma || g_pending_frame.luma_filled;  // a chroma write without its luma is a stale association
}

// One plane write that got no frame, one reason each, so a black flash can be named from the log.
void note_no_frame(const Session& s, bool luma, const uint8_t* data) {
    if (!s.guest_timing) return;  // without a verified fetch every write arrives with no frame by construction
    static std::atomic<uint64_t> no_fetch{0}, other_film{0}, other_plane{0}, chroma_alone{0}, total{0};
    (g_pending_frame.file_handle == 0                     ? no_fetch
     : g_pending_frame.file_handle != s.file_handle      ? other_film
     : fetch_owns(s, luma, data)                         ? chroma_alone
                                                         : other_plane)
        .fetch_add(1, std::memory_order_relaxed);
    const uint64_t n = total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 60 == 0)
        log("movie: %llu plane writes had no frame: %llu no fetch, %llu other film, %llu other plane, %llu chroma alone",
            (unsigned long long)n, (unsigned long long)no_fetch.load(), (unsigned long long)other_film.load(),
            (unsigned long long)other_plane.load(), (unsigned long long)chroma_alone.load());
}

// Copies one plane of this film's current frame into the guest buffer, and answers whether it did.
//
// `reshape` lets a luma write restart this film's decoder at the write's size: the game's planes are authoritative for
// the film it is playing, and a film decoded at another size (the 1280x720 ones against 1600x900 planes) would
// otherwise never appear. Only a caller that found no film able to serve reaches that.
bool serve_plane(const std::shared_ptr<Session>& sp, bool luma, uint32_t tex_w, uint32_t tex_h, uint8_t* data,
                 uint64_t row_stride, bool reshape) {
    Session& s = *sp;
    // This film's own fills serialize; another film's do not wait. The shared_ptr keeps the session alive even if the
    // game closes it in the middle of this copy, and no container lock is held, so a close never waits for a decoder.
    std::lock_guard flk(s.fill_m);
    if (s.stopping) return false;
    if (luma && reshape && (s.out_w != tex_w || s.out_h != tex_h || s.rewind_requested)) {
        s.stop();
        {
            // Session::stop() is terminal — a closed movie leaves `stopping` set, and that is what ends the reader
            // thread. A respawn is not a close, so the flag has to be cleared before the new reader starts; without
            // this it returned at its first frame push and the session served one frame (or none) forever.
            std::lock_guard slk(s.m);
            // Under the lock: plane_candidates() ranks every film by its size from another thread.
            s.out_w = tex_w;
            s.out_h = tex_h;
            s.next_index = 0;
            s.current.clear();
            s.current_set = false;
            s.current_pts_us = kNoPts;
            s.t0_set = false;
            s.rewind_requested = false;
            g_pending_frame.luma_filled = false;
            s.stopping = false;
            s.eof = false;
            s.ring.clear();
        }
        if (!start_decoder(s)) {
            // No decoder, so this film can never serve again: keeps it out of the candidate list and lets the next
            // open evict it. start_decoder() left nothing behind to release.
            std::lock_guard slk(s.m);
            s.stopping = true;
            return false;
        }
        log("movie: restarted decoder at %ux%u%s", tex_w, tex_h,
            s.guest_timing ? " following guest presentation time" : "");
    }
    // The write has to be one of THIS movie's planes: luma at the frame's size, chroma at half of
    // it. The old guard used `tex_w < 1000` as a stand-in for that test, so the 800x450 chroma
    // plane of a 1600x900 movie was rejected every time and U/V stayed at their initialised 127 —
    // every movie played perfectly but in greyscale.
    if (!shape_matches(s, luma, tex_w, tex_h)) return false;
    std::vector<uint8_t> retired;
    if (luma) {
        // fill_m keeps this film's current image alive through the plane copy; producers only mutate ring, so they
        // need not wait for that copy or logging. Retire the previous latch outside the producer's queue lock too.
        std::lock_guard slk(s.m);
        const auto now = movie_timing::Clock::now();
        if (!s.guest_timing && !s.t0_set) {
            s.t0 = now;
            s.t0_set = true;
            s.space.notify_one();
        }
        const uint64_t target = s.guest_timing ? 0 : movie_timing::target(s.t0, now, s.fps);
        if (prune_frames(s)) s.space.notify_one();
        const bool due = !s.ring.empty() && (s.guest_timing
            ? s.ring.front().pts_us != kNoPts && s.ring.front().pts_us <= g_pending_frame.pts_us
            : s.ring.front().index <= target);
        if (due && (!s.current_set || s.current_index != s.ring.front().index)) {
            // The selected frame leaves the pending queue and becomes the Y/UV
            // latch without copying its payload. Producer pruning cannot touch
            // it, and later uploads retain it even at EOF or with only future
            // frames queued.
            retired = std::move(s.current);
            s.current = std::move(s.ring.front().nv12);
            s.current_index = s.ring.front().index;
            s.current_pts_us = s.ring.front().pts_us;
            s.current_set = true;
            s.ring.pop_front();
            s.space.notify_one();
        }
    }
    if (s.current.size() < size_t(s.out_w) * s.out_h * 3 / 2) return false;
    copy_plane(s, luma, tex_w, tex_h, data, row_stride);
    if (!luma) {
        s.fills_chroma.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_picture_handle = s.file_handle;  // this pair's chroma write must come from the same film
    if (s.guest_timing) g_pending_frame.luma_filled = true;
    const uint64_t k = s.fills_luma.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k == 1 || k % 300 == 0) {
        log("movie: %s luma plane fill #%llu (out %ux%u)", s.guest.c_str(), (unsigned long long)k, s.out_w, s.out_h);
        if (s.guest_timing)
            log("movie: guest presentation %lld us, decoded frame %llu at %lld us (handle %llu)",
                (long long)g_pending_frame.pts_us, (unsigned long long)s.current_index, (long long)s.current_pts_us,
                (unsigned long long)s.file_handle);
    }
    return true;
}


bool movie_fill(uint32_t format, uint32_t tex_w, uint32_t tex_h, uint32_t region_w, uint32_t region_h,
                uint8_t* data, uint64_t row_stride) {
    if (format != kFmtR8 && format != kFmtRG8) return false;
    if (region_w != tex_w || region_h != tex_h) return false;  // whole-plane writes only
    const bool luma = format == kFmtR8;
    if (!data || row_stride < (luma ? uint64_t(tex_w) : uint64_t(tex_w) * 2)) return false;
    if (!luma && (tex_h % 2 || tex_w % 2)) return false;

    // Several films can be open at once (the game pre-opens the next cutscene), so this write has to be attributed to
    // one of them: by the plane size it asks for and, on a guest-PTS build, by the fetch that is in flight.
    // plane_candidates() puts the best answer first and each film is offered the write in turn.
    const auto candidates = plane_candidates(luma, tex_w, tex_h);
    if (candidates.empty()) return false;

    // 1) A film already producing exactly this plane. No decoder is restarted to answer a write that one frame of
    //    another film can cover: a handover must not black out the picture the game still holds.
    //
    //    Without a verified fetch (Android, NX_MOVIE_WALLCLOCK) a film's clock starts at the first plane it serves, so
    //    a pre-opened film must not answer just because the playing one has no decoded frame yet (its first frame is
    //    still in the decoder): that would start the next cutscene's clock early, under the current one's sound —
    //    issue #3 again. Such a write stays unserved for that moment instead.
    bool turn_taken = false;  // an older wall-clock film of this shape is still playing
    for (const auto& cand : candidates) {
        if (!shape_matches(*cand, luma, tex_w, tex_h)) continue;
        if (luma && cand->rewind_requested) continue;  // only the reshape below can serve that
        if (!fetch_owns(*cand, luma, data)) continue;
        bool wall_clock = false, started = false, playing = false;
        {
            std::lock_guard slk(cand->m);
            wall_clock = !cand->guest_timing;
            started = cand->t0_set;
            playing = !cand->stopping && !cand->loop && !(cand->eof && cand->ring.empty());
        }
        if (wall_clock && !started && turn_taken) continue;
        if (serve_plane(cand, luma, tex_w, tex_h, data, row_stride, false)) return true;
        if (wall_clock && playing) turn_taken = true;
    }

    // 2) No film of the right shape has a frame. The game's planes are authoritative for the film it is playing, so
    //    that one is re-encoded at their size (luma only: the chroma half skips until its session is ready).
    if (luma)
        for (const auto& cand : candidates) {
            if (cand->stopping) continue;
            // A finished film is only restarted for its own rewind: on a guest-PTS build that is the playing film being
            // asked to replay from an earlier presentation time. Without the rewind, restarting it would replay an
            // unrelated film's frames into the plane the game is showing.
            if (cand->eof && !cand->rewind_requested) continue;
            if (!fetch_owns(*cand, luma, data)) continue;
            if (shape_matches(*cand, luma, tex_w, tex_h) && !cand->rewind_requested) continue;
            if (serve_plane(cand, luma, tex_w, tex_h, data, row_stride, true)) return true;
        }

    // The guest buffer still holds its blank planes (Y 0, UV 127, pure black), so this write must not reach the
    // texture on its own; movie_plane_fallback may repeat the last picture instead.
    note_no_frame(*candidates.front(), luma, data);
    return false;
}

}  // namespace nx
