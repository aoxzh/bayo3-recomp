// Exercise the production WorkPool and write_backs_apply, using the existing
// compute-keep harness's Vulkan/upload mocks and independent merge oracle.
#define main compute_keep_harness_main
#include "nvn_compute_keep_test.cpp"
#undef main

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
using namespace std::chrono_literals;

uint64_t marker(uint64_t generation, uint64_t index) {
    return (generation + 1) * 0x9e3779b97f4a7c15ULL ^ (index + 1);
}

// Block every helper inside a distinct job, while the caller completes its own
// job. run() must wait even when all indices have already been claimed.
void delayed_helpers(unsigned helpers) {
    if (!helpers) return;
    auto& pool = nx::nvn::work_pool();
    const size_t count = size_t(helpers) + 1;
    std::vector<std::atomic<unsigned>> visits(count);
    for (auto& v : visits) v.store(0, std::memory_order_relaxed);
    std::vector<uint64_t> values(count);
    std::mutex mutex;
    std::condition_variable cv;
    bool caller_waiting = false, release_caller = false;
    bool release_helpers = false, returned = false;
    unsigned helpers_waiting = 0, completed = 0;

    std::thread caller([&] {
        const auto caller_id = std::this_thread::get_id();
        pool.run(count, [&](uint64_t i) {
            assert(i < count);
            const bool is_caller = std::this_thread::get_id() == caller_id;
            {
                std::unique_lock lock(mutex);
                if (is_caller) {
                    caller_waiting = true;
                    cv.notify_all();
                    cv.wait(lock, [&] { return release_caller; });
                } else {
                    ++helpers_waiting;
                    cv.notify_all();
                    cv.wait(lock, [&] { return release_helpers; });
                }
            }
            if (visits[i].fetch_add(1, std::memory_order_relaxed) == 0) values[i] = marker(17, i);
            {
                std::lock_guard lock(mutex);
                ++completed;
                cv.notify_all();
            }
        });
        std::lock_guard lock(mutex);
        returned = true;
        cv.notify_all();
    });

    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 5s, [&] { return caller_waiting && helpers_waiting == helpers; }));
        release_caller = true;
        cv.notify_all();
        assert(cv.wait_for(lock, 5s, [&] { return completed == 1; }));
        // All tasks are claimed; the helper callbacks still cannot complete.
        assert(!cv.wait_for(lock, 100ms, [&] { return returned; }));
        release_helpers = true;
        cv.notify_all();
    }
    caller.join();
    assert(returned && completed == count);
    for (size_t i = 0; i < count; ++i) {
        assert(visits[i].load(std::memory_order_relaxed) == 1);
        assert(values[i] == marker(17, i));
    }
}

void rapid_generations() {
    auto& pool = nx::nvn::work_pool();
    const size_t counts[] = {0, 1, 2, 7, 63, 1024, 3, 257};
    for (uint64_t generation = 0; generation < 1600; ++generation) {
        const size_t count = counts[generation % std::size(counts)];
        std::vector<std::atomic<unsigned>> visits(count);
        for (auto& v : visits) v.store(0, std::memory_order_relaxed);
        std::vector<uint64_t> values(count);
        std::atomic<bool> out_of_range{false};
        pool.run(count, [&](uint64_t i) {
            if (i >= count) {
                out_of_range.store(true, std::memory_order_relaxed);
                return;
            }
            if ((generation + i) % 13 == 0) std::this_thread::yield();
            // Only the first claimant writes this non-atomic slot, so an
            // erroneous duplicate is still counted without causing a test race.
            if (visits[i].fetch_add(1, std::memory_order_relaxed) == 0)
                values[i] = marker(generation, i);
        });
        assert(!out_of_range.load(std::memory_order_relaxed));
        for (size_t i = 0; i < count; ++i) {
            assert(visits[i].load(std::memory_order_relaxed) == 1);
            // The pool's completion barrier must make callback writes visible.
            assert(values[i] == marker(generation, i));
        }
    }
}

void many_production_writebacks() {
    constexpr size_t ranges = 48, size = 65536 + 19, stride = size + 64;
    std::vector<uint8_t> guest(ranges * stride, 0x31);
    std::vector<std::vector<uint8_t>> expected;
    expected.reserve(ranges);
    replay::written_reset();
    for (size_t i = 0; i < ranges; ++i) {
        uint8_t* dst = guest.data() + i * stride;
        for (size_t j = 0; j < size; ++j) dst[j] = uint8_t(i * 17 + j * 31);
        const std::vector<uint8_t> before(dst, dst + size);
        auto up = replay::written_bind(VK_NULL_HANDLE, dst, size, size, false);
        dst[0] ^= 0x21;       // CPU-only word survives
        dst[8] ^= 0x42;       // GPU wins the conflicting word
        up.ptr[4] ^= 0x83;
        up.ptr[8] ^= 0x14;
        up.ptr[size - 1] ^= 0x88;  // unaligned tail is merged bytewise
        expected.emplace_back(dst, dst + size);
        oracle(expected.back(), before, up.ptr);
    }
    auto backs = replay::written_take();
    assert(backs.size() == ranges);
    nx::nvn::write_backs_apply(backs);
    for (size_t i = 0; i < ranges; ++i)
        assert(std::memcmp(guest.data() + i * stride, expected[i].data(), size) == 0);

    // One large snapshot is split into many jobs, exercising concurrent slices
    // of the same production writeback as well as independent ranges above.
    std::vector<uint8_t> large(7 * 1024 * 1024 + 19, 0x56);
    const auto before = large;
    auto up = bind(large);
    for (size_t i = 0; i + 16 < large.size(); i += 4096) {
        large[i] ^= 0x11;
        large[i + 8] ^= 0x22;
        up.ptr[i + 4] ^= 0x33;
        up.ptr[i + 8] ^= 0x44;
    }
    up.ptr[large.size() - 1] ^= 0x55;
    auto want = large;
    oracle(want, before, up.ptr);
    land();
    assert(large == want);
}
}  // namespace

int main() {
    const char* e = std::getenv("NX_COPY_THREADS");
    assert(e);
    const unsigned helpers = unsigned(std::strtoul(e, nullptr, 10));
    assert(helpers <= 12);
    auto& pool = nx::nvn::work_pool();
    pool.run(0, [](uint64_t) { assert(false && "zero jobs invoked callback"); });
    delayed_helpers(helpers);
    rapid_generations();
    many_production_writebacks();
    std::printf("PASS writeback pool, %u helpers\n", helpers);
}
