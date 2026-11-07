// Software movie playback for the faked nvdec path (see movie.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace nx {

// The fs layer calls this when the game opens a romfs movie, with the guest path it used and the
// host file it resolved to. The unique FS handle identifies this open, including
// consecutive opens of the same path. Replaces any session already running.
void movie_on_open(const std::string& guest_path, const std::string& host_path, uint64_t file_handle = 0);
// A close from an older open of the same path must not stop its replacement.
void movie_on_close(const std::string& guest_path, uint64_t file_handle = 0);

// The verified guest frame-fetch call supplies selected microseconds and the
// exact plane destinations. Called on the renderer thread; zero handle clears
// its pending capture before each attempt. No guest object pointers are retained.
void movie_on_frame(uint64_t file_handle, int64_t pts_us, uint64_t luma, uint64_t chroma);

// Called from nvnTextureWriteTexels before the texels are copied out of guest memory: when the
// write is one of the active movie's Y/UV planes, the real decoded frame is written into the
// guest buffer first, so the game's own upload/composite path carries it from there. Returns
// whether a frame was supplied.
bool movie_fill(uint32_t format, uint32_t tex_w, uint32_t tex_h, uint32_t region_w, uint32_t region_h,
                uint8_t* data, uint64_t row_stride);

// After movie_fill refused a write. The write still carries the guest's blank planes, so if it is a plane of the active
// guest-timed movie the newest decoded frame goes into the guest buffer again (Repeated). Keeping what the texture held
// instead is not the same thing: the game cycles through several textures, so that showed a frame from a few frames back,
// and the picture jumped. Nothing: no frame yet, the write must not reach the texture. NotOurs: an ordinary upload.
enum class MoviePlane { NotOurs, Repeated, Nothing };
MoviePlane movie_plane_fallback(uint32_t format, uint32_t tex_w, uint32_t tex_h, uint8_t* data, uint64_t row_stride);

// ---- The guest movie layer's memory resource, read-only ----------------------------------------------------------
//
// docs/archive/MOVIE-EV0005.md §21. Everything the game's movie layer allocates — the FileSource preload window, the
// Extractor/Player objects, the codec's sample and frame buffers — comes out of one guest-side pool allocator: the
// resource the reader keeps at [reader+0x28] (a copy of the global at main+0x24ac628, code_269.cpp:9445-9447).
//
// The allocator is main-module code, so the runtime cannot free or steer it; it can only *read* it, which is what
// the ledger in hle_movie_api.cpp does (one line per movie open, one per close). The walk is pure bookkeeping, so
// it lives here and tests/movie_ledger_test.cpp drives it with synthetic guest memory: the point of the test is
// that the walk cannot loop forever, cannot dereference an address it was not told is readable, and can never
// report more than it actually read.
namespace movie_ledger {

// Node layout, from the create path main_00CD1F9C (code_269.cpp:4989-5079): it stores the owning resource at
// node+0x10 and the requested size at node+0x38, pushes the node onto [resource+0x68] newest-first through
// node+0x20 (node+0x18 points the other way) and bumps the live count at [resource+0x80]. The release path
// main_00CD3B20 (code_269.cpp:6996-7046) is the exact inverse, so both fields are maintained by the guest itself.
constexpr uint64_t kResourceListOffset = 0x68;   // [resource+0x68] = newest live node
constexpr uint64_t kResourceCountOffset = 0x80;  // [resource+0x80] = live allocation count
constexpr uint64_t kNodeOlderOffset = 0x20;      // [node+0x20] = the next older node
constexpr uint64_t kNodeSizeOffset = 0x38;       // [node+0x38] = the size the caller asked for
// The pool's own bounds, from the size-class insert main_00CD354C (code_269.cpp:6559-6651): +0x48 is the arena end
// the free-list insert compares against (`cmp x9, x10` right after adding a block's length), +0x40 is read with
// `cmp x8, x20; b.hs` when a block is returned, i.e. the advancing top of the tail. Both are only trusted when
// 0 < top <= end: a nonsense pair means the fields moved with a game update and the ledger says so instead of
// printing noise.
constexpr uint64_t kPoolTopOffset = 0x40;
constexpr uint64_t kPoolEndOffset = 0x48;
// A "size" at or above this is a pointer or a vtable word, not a requested size: never summed (the ledger has
// always applied this rule; it is what makes a corrupt node cost a wrong bucket instead of a nonsense total).
constexpr unsigned long long kPlausibleSizeLimit = 1ull << 40;
// Walk cap. The live set is a few thousand nodes and grows by ~1-4k per movie, and it has already outgrown the old
// 8192: `logs/ch1/base-1-game.log` at the fifth movie reported "8352 live allocations, 8192 walked ... largest
// 2101248 B (walk truncated ...)", i.e. the cap was silently turning the bytes into a lower bound and hiding the
// 8 MiB blocks behind the newest 8192 nodes. 32768 leaves room for ~10 movies at the observed rate, and one walk is
// still ~32k validated reads (well under a millisecond), which happens once per movie open and once per close, never
// per frame.
constexpr int kWalkCap = 32768;

// Size classes: dense at the bottom, because the growth in the logs is thousands of *small* buffers per movie
// (docs/archive/MOVIE-EV0005.md §21.2). bucket 0 is "<1K", then "<4K", "<16K", "<64K", "<256K", "<1M", "<4M", "<16M",
// and the last one is ">=16M".
constexpr int kBuckets = 9;

inline const char* bucket_label(int bucket) {
    static const char* const labels[kBuckets] = {"<1K", "<4K", "<16K", "<64K", "<256K", "<1M", "<4M", "<16M", ">=16M"};
    return bucket >= 0 && bucket < kBuckets ? labels[bucket] : "?";
}

inline int bucket_of(unsigned long long size) {
    static const unsigned long long bounds[kBuckets - 1] = {1ull << 10, 1ull << 12, 1ull << 14, 1ull << 16,
                                                            1ull << 18, 1ull << 20, 1ull << 22, 1ull << 24};
    for (int i = 0; i < kBuckets - 1; i++)
        if (size < bounds[i]) return i;
    return kBuckets - 1;
}

// What one ledger line reports. `declared` is the resource's own count; `walked` is what the list actually yielded,
// so `disagrees()` is the honesty flag: it is true when the walk stopped early (bad link or cap) or when the count
// moved under the walk, and in both cases the byte sum is a lower bound, not a measurement.
struct Snapshot {
    uint32_t declared = 0;
    int walked = 0;
    unsigned long long bytes = 0;
    unsigned long long largest = 0;
    bool truncated = false;
    int bucket_count[kBuckets] = {};
    unsigned long long bucket_bytes[kBuckets] = {};
    bool disagrees() const { return truncated || int(declared) != walked; }
};

// Walks the live list from `head` (newest first) for at most `cap` nodes. `read_u64(addr)` is called only for
// addresses `mapped(addr)` reports readable, so a corrupt link ends the walk instead of faulting the run.
template <class ReadU64, class Mapped>
Snapshot walk(uint64_t head, uint32_t declared, int cap, ReadU64 read_u64, Mapped mapped) {
    Snapshot s;
    s.declared = declared;
    if (cap > 0) {
        uint64_t node = head;
        while (node && s.walked < cap && mapped(node)) {
            const unsigned long long size = read_u64(node + kNodeSizeOffset);
            if (size < kPlausibleSizeLimit) {
                s.bytes += size;
                if (size > s.largest) s.largest = size;
                const int b = bucket_of(size);
                ++s.bucket_count[b];
                s.bucket_bytes[b] += size;
            }
            ++s.walked;
            node = read_u64(node + kNodeOlderOffset);
        }
        s.truncated = node != 0;
    } else {
        s.truncated = head != 0;
    }
    return s;
}

// Bytes the pool can still hand out of its tail, i.e. what a `main_00CD39BC` request that no free-list bin can
// serve has to come from. False when the two fields do not form a sane (top, end) pair — in that case the ledger
// prints them raw and claims nothing.
inline bool pool_room(uint64_t top, uint64_t end, unsigned long long* room) {
    if (!top || !end || top > end || end >= kPlausibleSizeLimit) return false;
    if (room) *room = end - top;
    return true;
}

}  // namespace movie_ledger
}  // namespace nx
