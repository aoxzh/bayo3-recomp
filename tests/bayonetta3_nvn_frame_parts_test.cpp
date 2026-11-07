// Exercise the production capacity policy without a Vulkan device or game.
#include "../runtime/src/nvn/nvn_frame_parts.h"
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace nx::nvn;
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

void check_sequence(uint32_t limit, int split_after) {
    uint32_t part = 0, queued = 0;
    bool pending = split_after >= 0;
    auto queue_part = [&] {
        check(part < limit && part < kFrameMaxParts && part < 32, "part and bitmap bounds");
        check(2 * part + 1 < 2 * limit, "query pair stays in active pool");
        check(!(queued & (1u << part)), "part queued once");
        queued |= 1u << part;
    };
    while (pending && part < uint32_t(split_after) && frame_chunk_has_room(part, limit, true)) {
        queue_part();
        ++part;
    }
    if (pending) {
        check(part + 1 < limit, "late split retains its successor");
        queue_part();
        ++part;
        pending = false;
    }
    while (frame_chunk_has_room(part, limit, pending)) {
        queue_part();
        ++part;
    }
    queue_part();  // the final part is always allocated, distinct and in order
    check(queued == (1u << (part + 1)) - 1, "queued parts are consecutive");
    if (limit > kFrameDefaultParts) check(part == limit - 1, "opt-in uses final slot");
    else check(part == (split_after >= 6 ? 7u : 6u), "default partition unchanged");
}
}

int main() {
    try {
        check(frame_parts_from_env(nullptr) == 8, "unset retains default");
        for (const char* value : {"", "0", "7", "8", "-1", "17", "32", "16x", "16 ", "junk",
                                  "99999999999999999999999999999999999999999999"})
            check(frame_parts_from_env(value) == 8, "invalid or explicit8 retains default");
        for (uint32_t limit = 8; limit <= kFrameMaxParts; ++limit) {
            check(frame_parts_from_env(std::to_string(limit).c_str()) == limit, "accepted cap");
            for (uint32_t part = 0; part <= kFrameMaxParts; ++part) {
                if (limit == 8) {
                    check(frame_chunk_has_room(part, limit, false) == (part + 2 < 8), "default post-split guard");
                    check(frame_chunk_has_room(part, limit, true) == (part + 2 < 8), "default pending-split guard");
                }
                if (frame_chunk_has_room(part, limit, true))
                    check(part + 2 < limit, "chunk leaves both split and final room");
                if (frame_chunk_has_room(part, limit, false))
                    check(part + 1 < limit, "chunk leaves final room");
            }
            check(!frame_chunk_has_room(UINT32_MAX, limit, false), "overflowing input is rejected");
            check_sequence(limit, -1);  // no planned split
            for (int split = 0; split <= int(limit + 1); ++split)
                check_sequence(limit, split);  // early, middle and latest possible split
        }
        std::puts("PASS frame part defaults, strict cap parsing, split/final capacity and query/bitmap bounds");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
