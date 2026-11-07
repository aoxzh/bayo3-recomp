#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdlib>

namespace nx::nvn {

inline constexpr uint32_t kFrameDefaultParts = 8;
inline constexpr uint32_t kFrameMaxParts = 16;
static_assert(kFrameMaxParts <= 32);  // Slot::queued uses one uint32_t bit per part.

// Read once at frame_init. Unset, 8 and invalid values preserve the original
// eight-part allocation and unconditional two-part reservation.
inline uint32_t frame_parts_from_env(const char* value) {
    if (!value || !*value) return kFrameDefaultParts;
    char* end = nullptr;
    errno = 0;
    const long requested = std::strtol(value, &end, 10);
    if (errno || end == value || *end || requested < kFrameDefaultParts || requested > kFrameMaxParts)
        return kFrameDefaultParts;
    return uint32_t(requested);
}

// A chunk queues the current part and starts another one. While a split is
// pending that next part must still have a successor for the post-split work.
// The opt-in budget can use the last slot once no split remains; default8 keeps
// the historical guard even after the split, for an unchanged control arm.
inline constexpr bool frame_chunk_has_room(uint32_t part, uint32_t limit, bool split_pending) {
    const uint32_t reserved = limit > kFrameDefaultParts && !split_pending ? 1 : 2;
    return part < limit && reserved < limit - part;
}

// Active allocation count, initialized before any slot or timestamp pool is used.
uint32_t frame_part_count();

}  // namespace nx::nvn
