// Exercise the actual producer, consumer, and child lifecycle without a game or ffmpeg.
#include "../runtime/src/movie.cpp"

#include <algorithm>
#include <cassert>
#include <climits>
#include <filesystem>
#include <iostream>

namespace nx {
void log(const char*, ...) {}
}

using namespace std::chrono_literals;
using nx::movie_timing::Clock;

static std::vector<uint8_t> frame(uint8_t value, unsigned w = 4, unsigned h = 4) {
    std::vector<uint8_t> bytes(size_t(w) * h * 3 / 2, uint8_t(value + 50));
    std::fill(bytes.begin(), bytes.begin() + size_t(w) * h, value);
    return bytes;
}

// Several movies can be open at once, so the tests drive the container the runtime keeps for them directly:
// these tests never open a real file, they install a Session and then exercise the plane-fill path against it.
// Both also drop this thread's fetch in flight: a fetch claims every write until the next one, so a fetch left over
// from an earlier test would claim the next test's wall-clock writes.
static void install_session(std::unique_ptr<nx::Session> s) {
    nx::g_pending_frame = {};
    std::lock_guard lk(nx::g_movie);
    nx::g_sessions.clear();
    nx::g_sessions.push_back(std::shared_ptr<nx::Session>(std::move(s)));
}
static nx::Session* only_session() {
    std::lock_guard lk(nx::g_movie);
    return nx::g_sessions.empty() ? nullptr : nx::g_sessions.front().get();
}
static void clear_sessions() {
    nx::g_pending_frame = {};
    std::lock_guard lk(nx::g_movie);
    nx::g_sessions.clear();
}

template <class Predicate>
static void eventually(Predicate pred) {
    const auto deadline = Clock::now() + 3s;
    while (!pred()) {
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

static void test_clock_and_pruning() {
    using namespace nx::movie_timing;
    const auto zero = Clock::time_point{};
    for (double fps : {24.0, 30.0, 29.97, 30000.0 / 1001.0}) {
        for (uint64_t i : {1ull, 2ull, 17ull, 300ull, 10000ull}) {
            const auto due = deadline(zero, i, fps);
            assert(target(zero, due, fps) == i);
            assert(target(zero, due - Clock::duration(1), fps) == i - 1);
        }
    }
    assert(deadline(zero, UINT64_MAX, 0.0) == Clock::time_point::max());
    assert(deadline(zero, UINT64_MAX, 1.0) == Clock::time_point::max());
    std::deque<nx::Frame> q;
    for (uint64_t i = 0; i < 8; ++i) q.push_back({i, {}});
    assert(prune_due(q, 4));
    assert(q.size() == 4 && q.front().index == 4 && q.back().index == 7);
    assert(!prune_due(q, 4));
    assert(prune_due(q, 99));
    assert(q.size() == 1 && q.front().index == 7);
    assert(!prune_due(q, 999));
    q = {{0, {}, 2000000}, {1, {}, 2125000}, {2, {}, 2500000}, {3, {}, 2875000}};
    assert(!prune_pts(q, 1999999));
    assert(prune_pts(q, 2400000));
    assert(q.front().index == 1 && q.size() == 3);
    assert(!prune_pts(q, 2400000));
    assert(prune_pts(q, 2875000));
    assert(q.size() == 1 && q.front().index == 3);
}

static void test_preroll_and_late_producer() {
    nx::Session s;
    s.fps = 30;
    // A late producer must keep catching up instead of blocking after 8 frames.
    {
        std::lock_guard lk(s.m);
        s.t0 = Clock::now() - 10s;
        s.t0_set = true;
    }
    for (int i = 0; i < 100; ++i) assert(nx::enqueue_frame(s, frame(uint8_t(i))));
    {
        std::lock_guard lk(s.m);
        assert(s.ring.size() == 1 && s.ring.front().index == 99 && s.next_index == 100);
    }

    nx::Session pre;
    pre.fps = 60;
    for (int i = 0; i < 8; ++i) assert(nx::enqueue_frame(pre, frame(uint8_t(i))));
    std::atomic<bool> enqueued{false};
    std::thread producer([&] { enqueued = nx::enqueue_frame(pre, frame(8)); });
    std::this_thread::sleep_for(30ms);
    assert(!enqueued.load());  // no playback origin yet
    {
        std::lock_guard lk(pre.m);
        pre.t0 = Clock::now();
        pre.t0_set = true;
    }
    pre.space.notify_one();
    eventually([&] { return enqueued.load(); });  // wait_until must wake on the next due frame
    producer.join();
    {
        std::lock_guard lk(pre.m);
        assert(pre.ring.size() <= 8 && pre.ring.front().index >= 1 && pre.ring.back().index == 8);
    }

    nx::Session stopped;
    for (int i = 0; i < 8; ++i) assert(nx::enqueue_frame(stopped, frame(uint8_t(i))));
    std::atomic<bool> returned{false};
    stopped.reader = std::thread([&] { assert(!nx::enqueue_frame(stopped, frame(8))); returned = true; });
    stopped.stop();
    assert(returned.load());
}

static void test_luma_chroma_latch() {
    auto s = std::make_unique<nx::Session>();
    s->out_w = 4;
    s->out_h = 4;
    s->fps = 1;
    s->ring.push_back({0, frame(1)});
    s->ring.push_back({1, frame(2)});
    s->ring.push_back({2, frame(3)});
    s->ring.push_back({100, frame(4)});
    install_session(std::move(s));
    {
        std::lock_guard lk(only_session()->m);
        only_session()->t0 = Clock::now() - 2s;
        only_session()->t0_set = true;
    }
    uint8_t y[16] = {}, uv[8] = {};
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 3);
    {
        std::lock_guard lk(only_session()->m);
        nx::movie_timing::prune_due(only_session()->ring, 100);  // producer advances between the plane writes
    }
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 53);
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));  // future-only frame must not replace the latch
    for (auto v : y) assert(v == 3);
    {
        std::lock_guard lk(only_session()->m);
        only_session()->t0 = Clock::now() - 101s;
    }
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : y) assert(v == 4);
    for (auto v : uv) assert(v == 54);
    clear_sessions();

    auto first = std::make_unique<nx::Session>();
    first->out_w = 4;
    first->out_h = 4;
    first->ring.push_back({0, frame(9)});
    install_session(std::move(first));
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    {
        std::lock_guard lk(only_session()->m);
        assert(only_session()->t0_set && only_session()->current_index == 0);
    }
    clear_sessions();
}

static void test_latch_ownership_and_backpressure() {
    auto s = std::make_unique<nx::Session>();
    s->out_w = s->out_h = 4;
    s->fps = 0.001;  // The next frame cannot become due during this test.
    for (int i = 0; i < 8; ++i) assert(nx::enqueue_frame(*s, frame(uint8_t(i))));
    const uint8_t* first_payload = s->ring.front().nv12.data();
    install_session(std::move(s));
    nx::Session& session = *only_session();
    std::atomic<bool> entered{false}, enqueued{false};
    std::thread producer([&] {
        entered = true;
        enqueued = nx::enqueue_frame(session, frame(8));
    });
    eventually([&] { return entered.load(); });
    std::this_thread::sleep_for(30ms);
    assert(!enqueued.load());

    uint8_t y[16] = {}, uv[8] = {};
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    // The original allocation is now owned by the latch; no second payload was
    // allocated/copied. Consuming it must wake the blocked producer even though
    // playback is still at frame zero.
    assert(session.current.data() == first_payload && session.current_index == 0);
    eventually([&] { return enqueued.load(); });
    producer.join();
    {
        std::lock_guard lk(session.m);
        assert(session.ring.size() == 8);
        assert(session.ring.front().index == 1 && session.ring.back().index == 8);
    }
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 50);
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    assert(session.current.data() == first_payload);
    for (auto v : y) assert(v == 0);  // Future frames cannot replace the latch.

    const uint8_t* final_payload;
    {
        std::lock_guard lk(session.m);
        final_payload = session.ring.back().nv12.data();
        session.t0 = Clock::now() - 9000s;
        session.eof = true;
    }
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    assert(session.current.data() == final_payload && session.current_index == 8);
    assert(session.ring.empty());
    for (int repeat = 0; repeat < 3; ++repeat) {
        assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
        assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
        for (auto v : y) assert(v == 8);
        for (auto v : uv) assert(v == 58);
        assert(session.current.data() == final_payload);
    }
    clear_sessions();
}

static void test_guest_pts_and_identity() {
    auto s = std::make_unique<nx::Session>();
    s->out_w = s->out_h = 4;
    s->guest = "same-file";
    s->file_handle = 101;
    s->fps = 123;  // Deliberately unrelated to this VFR, nonzero-origin timeline.
    s->t0 = Clock::now() - 10h;
    s->ring = {{0, frame(10), 2000000}, {1, frame(11), 2125000},
               {2, frame(12), 2500000}, {3, frame(13), 2875000}};
    install_session(std::move(s));
    uint8_t y[16] = {}, uv[8] = {}, other[16] = {};
    const auto capture = [&](int64_t pts) {
        nx::movie_on_frame(101, pts, uintptr_t(y), uintptr_t(uv));
    };
    capture(2400000);
    assert(!nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));  // UV cannot precede its Y.
    assert(nx::movie_plane_fallback(0xd, 2, 2, uv, 4) == nx::MoviePlane::Nothing);  // No frame yet: not uploaded.
    assert(!nx::movie_fill(1, 4, 4, 4, 4, other, 4));
    assert(nx::movie_plane_fallback(1, 4, 4, other, 4) == nx::MoviePlane::Nothing);
    assert(nx::movie_plane_fallback(1, 8, 8, other, 8) == nx::MoviePlane::NotOurs);  // Another size.
    assert(nx::movie_plane_fallback(0x25, 4, 4, other, 4) == nx::MoviePlane::NotOurs);  // Another format.
    assert(!nx::movie_fill(1, 4, 4, 4, 4, y, 3));
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 11);
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 61);
    assert(!only_session()->t0_set && only_session()->current_pts_us == 2125000);

    // A guest pause cannot advance selection merely because host time passes.
    std::this_thread::sleep_for(30ms);
    capture(2400000);
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 11);
    capture(2875000);
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 13);
    {
        std::lock_guard lk(only_session()->m);
        only_session()->ring.push_back({4, frame(14), 3200000});
        nx::movie_timing::prune_pts(only_session()->ring, 3200000);
    }
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 63);  // Producer pruning cannot change the Y/UV latch.
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 13);  // The only queued frame is still in the future.

    std::thread unrelated([&] {
        nx::movie_on_frame(0, -1, 0, 0);
        assert(!nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    });
    unrelated.join();
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));  // Clearing another thread is isolated.
    nx::movie_on_frame(0, -1, 0, 0);
    assert(!nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    assert(!nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    // A failed fetch repeats the newest frame, Y and UV of the same picture, without advancing it.
    std::fill(std::begin(y), std::end(y), uint8_t(0)), std::fill(std::begin(uv), std::end(uv), uint8_t(0));
    assert(nx::movie_plane_fallback(1, 4, 4, y, 4) == nx::MoviePlane::Repeated);
    assert(nx::movie_plane_fallback(0xd, 2, 2, uv, 4) == nx::MoviePlane::Repeated);
    for (auto v : y) assert(v == 13);
    for (auto v : uv) assert(v == 63);
    assert(only_session()->current_pts_us == 2875000);
    nx::movie_on_frame(100, 9999999, uintptr_t(y), uintptr_t(uv));
    assert(only_session()->target_pts_us == 2875000);
    assert(!nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    nx::movie_on_close("same-file", 100);
    assert(only_session());  // Closing an earlier open of this path must not close this one.
    nx::movie_on_close("same-file", 101);
    assert(!only_session());

    auto reopened = std::make_unique<nx::Session>();
    reopened->guest = "same-file";
    reopened->file_handle = 102;
    install_session(std::move(reopened));
    capture(3200000);
    assert(!only_session()->guest_timing && !nx::g_pending_frame.file_handle);
    nx::movie_on_close("same-file", 101);
    assert(only_session());
    nx::movie_on_close("same-file", 102);
    assert(!only_session());
}

static void test_guest_producer_pause() {
    nx::Session s;
    s.guest_timing = true;
    s.target_pts_us = 2000000;
    s.t0_set = true;
    s.t0 = Clock::now() - 10h;  // A stale fallback clock cannot discard paused guest frames.
    for (int i = 0; i < 8; ++i)
        assert(nx::enqueue_frame(s, frame(uint8_t(i)), 2000000 + i * 100000));
    std::atomic<bool> done{false};
    std::thread producer([&] { done = nx::enqueue_frame(s, frame(8), 2800000); });
    std::this_thread::sleep_for(30ms);
    assert(!done.load());
    {
        std::lock_guard lk(s.m);
        assert(s.ring.size() == 8 && s.ring.front().index == 0);
        s.target_pts_us = 2700000;
    }
    s.space.notify_one();
    eventually([&] { return done.load(); });
    producer.join();
    assert(s.ring.size() == 2 && s.ring.front().pts_us == 2700000);
}

#if !defined(__ANDROID__)
static void assert_no_children() {
    int status;
    errno = 0;
    assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void test_guest_rewind_and_resize() {
    auto s = std::make_unique<nx::Session>();
    s->out_w = s->out_h = 2;
    s->file_handle = 201;
    s->host = "vfr";
    install_session(std::move(s));
    uint8_t y[16] = {}, uv[8] = {};
    nx::movie_on_frame(201, 2500000, uintptr_t(y), uintptr_t(uv));
    nx::movie_fill(1, 4, 4, 4, 4, y, 4);  // Resize while retaining the selected guest target.
    eventually([&] { std::lock_guard lk(only_session()->m); return only_session()->next_index == 4; });
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 12);
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 62);
    assert(only_session()->target_pts_us == 2500000 && !only_session()->t0_set);

    nx::movie_on_frame(201, 2000000, uintptr_t(y), uintptr_t(uv));
    assert(only_session()->rewind_requested);
    assert(!nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    nx::movie_fill(1, 4, 4, 4, 4, y, 4);
    eventually([&] { std::lock_guard lk(only_session()->m); return only_session()->next_index == 4; });
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 10);
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 60);
    assert(!only_session()->rewind_requested && only_session()->current_pts_us == 2000000);
    clear_sessions();
    assert_no_children();
}

static void test_stalled_upload_catches_up() {
    auto s = std::make_unique<nx::Session>();
    s->out_w = s->out_h = 4;
    s->host = "burst";
    s->fps = 30;
    assert(nx::start_decoder(*s));
    eventually([&] { std::lock_guard lk(s->m); return s->next_index == 8; });
    install_session(std::move(s));
    uint8_t y[16] = {}, uv[8] = {};
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 0);
    {
        std::lock_guard lk(only_session()->m);
        only_session()->t0 -= 3s;  // simulate a long period without uploads
    }
    only_session()->space.notify_one();
    eventually([&] { std::lock_guard lk(only_session()->m); return only_session()->next_index >= 95; });
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 50);  // producer advanced, but UV still pairs with the prior Y
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    uint64_t first_index;
    {
        std::lock_guard lk(only_session()->m);
        first_index = only_session()->current_index;
        assert(first_index >= 90 && only_session()->ring.size() <= 8);
        only_session()->t0 -= 1s;  // another low-cadence upload gap
    }
    only_session()->space.notify_one();
    eventually([&] { std::lock_guard lk(only_session()->m); return only_session()->next_index >= 125; });
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == uint8_t(first_index % 100 + 50));
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    {
        std::lock_guard lk(only_session()->m);
        assert(only_session()->current_index >= 120 && only_session()->current_index > first_index);
        for (auto v : y) assert(v == uint8_t(only_session()->current_index % 100));
    }
    clear_sessions();
    assert_no_children();
}

static void test_resize_resets_origin() {
    auto s = std::make_unique<nx::Session>();
    s->out_w = s->out_h = 2;
    s->host = "burst";
    s->fps = 1;
    s->next_index = 100;
    s->ring.push_back({99, frame(99, 2, 2)});
    s->current = frame(99, 2, 2);
    s->current_index = 99;
    s->current_set = true;
    s->t0_set = true;
    s->t0 = Clock::now() - 5min;
    install_session(std::move(s));
    const auto before = Clock::now();
    uint8_t y[16] = {};
    // The first fill starts the scaled decoder and restarts the luma origin,
    // even when its first frame has not arrived by the time this call returns.
    nx::movie_fill(1, 4, 4, 4, 4, y, 4);
    {
        std::lock_guard lk(only_session()->m);
        assert(only_session()->t0_set && only_session()->t0 >= before);
        assert(!only_session()->current_set || only_session()->current_index == 0);
    }
    eventually([&] { std::lock_guard lk(only_session()->m); return !only_session()->ring.empty(); });
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 0);
    clear_sessions();
    assert_no_children();
}

static void test_decoder_processes() {
    {
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.host = "block";
        s.stop();
        // Child creation finished after cancellation: it must be reaped rather
        // than published to a stopped session.
        assert(!nx::spawn_ffmpeg(s));
        assert_no_children();
    }
    {
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.host = "empty";
        s.loop = true;
        assert(nx::start_decoder(s));
        eventually([&] { std::lock_guard lk(s.m); return s.eof && s.ff == 0; });
        {
            std::lock_guard lk(s.m);
            assert(s.next_index == 0 && s.fd == -1);
        }
        s.stop();
        assert_no_children();
    }
    {
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.host = "once";
        s.loop = true;
        s.fps = 1;
        assert(nx::start_decoder(s));
        eventually([&] { std::lock_guard lk(s.m); return s.next_index == 8; });
        {
            std::lock_guard lk(s.m);
            assert(s.ring.size() == 8);
            s.t0 = Clock::now() - 10s;
            s.t0_set = true;
        }
        s.space.notify_one();
        eventually([&] { std::lock_guard lk(s.m); return s.next_index >= 11; });
        {
            std::lock_guard lk(s.m);
            assert(s.ring.size() <= 8 && s.ring.front().index >= 10);
            for (size_t i = 1; i < s.ring.size(); ++i)
                assert(s.ring[i].index == s.ring[i - 1].index + 1);
        }
        s.stop();
        assert_no_children();
    }
    {
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.host = "block";
        assert(nx::start_decoder(s));
        const auto start = Clock::now();
        s.stop();
        s.stop();
        assert(Clock::now() - start < 2s);
        assert_no_children();
    }
    // Stop repeatedly while the looping reader may be detaching or spawning.
    for (int i = 0; i < 20; ++i) {
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.host = "once";
        s.loop = true;
        assert(nx::start_decoder(s));
        eventually([&] { std::lock_guard lk(s.m); return s.next_index > 0; });
        s.stop();
        assert_no_children();
    }
}

static int fake_ffmpeg(int argc, char** argv) {
    std::string mode;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "-i") == 0) mode = argv[i + 1];
    if (mode == "empty") return 0;
    if (mode == "block") {
        for (;;) pause();
    }
    if (mode == "burst") {
        for (unsigned i = 0; i < 160; ++i) {
            auto bytes = frame(uint8_t(i % 100));
            dprintf(STDERR_FILENO, "NXPTS %u %u 1/30 %zu\n", i, i, bytes.size());
            if (write(STDOUT_FILENO, bytes.data(), bytes.size()) != ssize_t(bytes.size())) return 1;
        }
        return 0;
    }
    if (mode == "vfr") {
        constexpr unsigned pts[] = {2000, 2125, 2500, 2875};
        for (unsigned i = 0; i < 4; ++i) {
            auto bytes = frame(uint8_t(10 + i));
            dprintf(STDERR_FILENO, "NXPTS %u %u 1/1000 %zu\n", i, pts[i], bytes.size());
            if (write(STDOUT_FILENO, bytes.data(), bytes.size()) != ssize_t(bytes.size())) return 1;
        }
        return 0;
    }
    if (mode == "once") {
        const uint8_t bytes[6] = {1, 1, 1, 1, 51, 51};
        dprintf(STDERR_FILENO, "NXPTS 0 0 1/30 %zu\n", sizeof bytes);
        return write(STDOUT_FILENO, bytes, sizeof bytes) == sizeof bytes ? 0 : 1;
    }
    return 2;
}

#else
struct AndroidState {
    std::string mode;
    std::atomic<int> calls{0}, rewinds{0}, stops{0};
    std::mutex m;
    std::condition_variable cv;
    bool stopped = false;
};
static std::shared_ptr<AndroidState> android_state;

class FakeAndroidStream final : public nx::movie_android::Stream {
    std::shared_ptr<AndroidState> s_;
public:
    explicit FakeAndroidStream(std::shared_ptr<AndroidState> s) : s_(std::move(s)) {}
    bool next(std::vector<uint8_t>& nv12, int64_t& pts_us) override {
        (void)pts_us;  // no timestamp: the reader keeps kNoPts
        const int call = s_->calls.fetch_add(1);
        if (s_->mode == "block") {
            std::unique_lock lk(s_->m);
            s_->cv.wait(lk, [&] { return s_->stopped; });
            return false;
        }
        if (s_->mode == "empty" || call % 2) return false;
        nv12 = frame(1, 2, 2);
        return true;
    }
    bool rewind() override { ++s_->rewinds; return true; }
    void stop() override {
        {
            std::lock_guard lk(s_->m);
            s_->stopped = true;
            ++s_->stops;
        }
        s_->cv.notify_all();
    }
    uint32_t width() const override { return 2; }
    uint32_t height() const override { return 2; }
};
namespace nx::movie_android {
bool probe(const std::string&, uint32_t&, uint32_t&, double&) { return false; }
std::unique_ptr<Stream> open(const std::string&, uint32_t, uint32_t) {
    return std::make_unique<FakeAndroidStream>(android_state);
}
}

static void test_android_stream() {
    {
        android_state = std::make_shared<AndroidState>();
        android_state->mode = "empty";
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.loop = true;
        assert(nx::start_decoder(s));
        eventually([&] { std::lock_guard lk(s.m); return s.eof; });
        assert(android_state->calls == 1 && android_state->rewinds == 0);
        s.stop();
        assert(android_state->stops == 1);
    }
    {
        android_state = std::make_shared<AndroidState>();
        android_state->mode = "loop";
        nx::Session s;
        s.out_w = s.out_h = 2;
        s.loop = true;
        s.fps = 1;
        assert(nx::start_decoder(s));
        eventually([&] { std::lock_guard lk(s.m); return s.next_index == 8; });
        {
            std::lock_guard lk(s.m);
            s.t0 = Clock::now() - 10s;
            s.t0_set = true;
        }
        s.space.notify_one();
        eventually([&] { std::lock_guard lk(s.m); return s.next_index >= 11; });
        assert(android_state->rewinds >= 10);
        s.stop();
        assert(android_state->stops == 1);
    }
    {
        android_state = std::make_shared<AndroidState>();
        android_state->mode = "block";
        nx::Session s;
        s.out_w = s.out_h = 2;
        assert(nx::start_decoder(s));
        eventually([&] { return android_state->calls > 0; });
        const auto start = Clock::now();
        s.stop();
        assert(Clock::now() - start < 2s && android_state->stops == 1);
    }
}
#endif

// Issue #3: the game pre-opens the next cutscene (ev0011) one frame into the current one (ev0010), and on a build
// without a verified fetch (Android) nothing but open order says which film is playing. The older film must keep the
// picture until it has played out, and the pre-opened film must then start from its own first frame.
static void test_preopened_film_waits_its_turn() {
    clear_sessions();
    auto playing = std::make_shared<nx::Session>();
    playing->file_handle = 718;
    playing->out_w = playing->out_h = 4;
    playing->fps = 1;
    playing->ring.push_back({0, frame(10)});
    playing->ring.push_back({1, frame(11)});
    auto next = std::make_shared<nx::Session>();
    next->file_handle = 730;
    next->out_w = next->out_h = 4;
    next->fps = 1;
    next->ring.push_back({0, frame(20)});
    next->ring.push_back({1, frame(21)});
    {
        std::lock_guard lk(nx::g_movie);
        nx::g_sessions.push_back(playing);
        nx::g_sessions.push_back(next);  // opened later: pre-opened
    }
    uint8_t y[16] = {}, uv[8] = {};
    std::deque<nx::Frame> held;
    {
        std::lock_guard lk(playing->m);
        held.swap(playing->ring);  // the playing film's first frame is still in its decoder
    }
    assert(!nx::movie_fill(1, 4, 4, 4, 4, y, 4));  // a blank moment, not the next film's clock started early
    {
        std::lock_guard lk(next->m);
        assert(!next->t0_set && next->fills_luma == 0);
    }
    {
        std::lock_guard lk(playing->m);
        held.swap(playing->ring);
    }
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 10);  // the playing film, not the one just opened
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 60);
    {
        std::lock_guard lk(next->m);
        assert(!next->t0_set);  // the pre-opened film's clock has not started
    }
    {
        std::lock_guard lk(playing->m);
        playing->eof = true;  // decoded to the end, but frame 11 is still to be shown
        playing->t0 = Clock::now() - 5s;
    }
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 11);  // decoder EOF alone does not hand over
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 20);  // played out: the next film, from its own first frame
    assert(nx::movie_fill(0xd, 2, 2, 2, 2, uv, 4));
    for (auto v : uv) assert(v == 70);  // the chroma half of the same pair
    assert(playing->fills_luma == 2 && next->fills_luma == 1);
    clear_sessions();
}

// A fetch for the playing film claims its writes, including the plane buffer it does not name: a pre-opened film the
// game has not fetched from yet must not answer one and start its clock (ev0022 on Android froze on its first frame).
static void test_preopened_film_ignores_claimed_writes() {
    clear_sessions();
    auto playing = std::make_shared<nx::Session>();
    playing->file_handle = 811;
    playing->out_w = playing->out_h = 4;
    playing->fps = 30;
    playing->ring.push_back({0, frame(10), 0});
    playing->ring.push_back({1, frame(11), 33333});
    auto next = std::make_shared<nx::Session>();
    next->file_handle = 812;
    next->out_w = next->out_h = 4;
    next->fps = 30;
    next->ring.push_back({0, frame(20), 0});
    next->ring.push_back({1, frame(21), 33333});
    {
        std::lock_guard lk(nx::g_movie);
        nx::g_sessions.push_back(playing);
        nx::g_sessions.push_back(next);
    }
    uint8_t y[16] = {}, uv[8] = {}, y2[16] = {};
    nx::movie_on_frame(811, 0, uintptr_t(y), uintptr_t(uv));
    assert(nx::movie_fill(1, 4, 4, 4, 4, y, 4));
    for (auto v : y) assert(v == 10);
    assert(!nx::movie_fill(1, 4, 4, 4, 4, y2, 4));  // the playing film's other buffer: claimed, not the next film's
    {
        std::lock_guard lk(next->m);
        assert(!next->t0_set && next->fills_luma == 0);
    }
    nx::movie_on_frame(812, 0, uintptr_t(y2), uintptr_t(uv));  // the game starts the next film: from its first frame
    assert(nx::movie_fill(1, 4, 4, 4, 4, y2, 4));
    for (auto v : y2) assert(v == 20);
    nx::movie_on_frame(0, -1, 0, 0);
    clear_sessions();
}

int main(int argc, char** argv) {
#if !defined(__ANDROID__)
    if (std::filesystem::path(argv[0]).filename() == "ffmpeg") return fake_ffmpeg(argc, argv);
    assert(argc == 2);
    assert(setenv("PATH", argv[1], 1) == 0);
#endif
    test_clock_and_pruning();
    test_preroll_and_late_producer();
    test_luma_chroma_latch();
    test_latch_ownership_and_backpressure();
    test_preopened_film_waits_its_turn();
    test_preopened_film_ignores_claimed_writes();
    test_guest_pts_and_identity();  // Android follows the game's fetch too
    test_guest_producer_pause();
#if defined(__ANDROID__)
    test_android_stream();
#else
    test_guest_rewind_and_resize();
    test_stalled_upload_catches_up();
    test_resize_resets_origin();
    test_decoder_processes();
#endif
    std::cout << "movie timing, latching and decoder lifecycle: PASS\n";
}
