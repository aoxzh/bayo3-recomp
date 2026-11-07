// Verified frame-fetch seam in the analyzed main executable (SHA-256
// 499983f6adabc071488be7fe75f547c079c8c8dac95c3573e78a4277d7641800).
// The original instruction bytes guard the game-specific register and field layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace nx::movie_frame_source {

constexpr uint64_t kReturnOffset = 0xcd79b0ull;

struct Capture {
    uint64_t file_handle = 0;
    int64_t pts_us = -1;
    uint64_t luma = 0;
    uint64_t chroma = 0;
    bool valid = false;
};

inline bool matching_caller(uint64_t module_base, uint64_t lr) {
    return module_base <= UINT64_MAX - kReturnOffset && lr == module_base + kReturnOffset;
}

// Each signature checks original, relocated-in-place code: frame setup and
// state transition, decoder call/result, both plane copies, timestamp and
// selection. A changed or unreadable code layout disables publication.
inline constexpr uint8_t kCode0[] = {0xf6, 0x03, 0x03, 0xaa, 0xf4, 0x03, 0x02, 0xaa, 0x7f, 0x00, 0x00, 0xb9, 0xf5, 0x03, 0x01, 0xaa, 0xf3, 0x03, 0x00, 0xaa, 0x19, 0x80, 0x0c, 0x91, 0x68, 0x00, 0x80, 0x52, 0x29, 0xff, 0x5f, 0x88, 0x3f, 0x09, 0x00, 0x71, 0x01, 0x0b, 0x00, 0x54, 0x28, 0xff, 0x09, 0x88};
inline constexpr uint8_t kCode1[] = {0x60, 0xb2, 0x40, 0xf9, 0xe2, 0x03, 0x00, 0x91, 0x61, 0x2a, 0x43, 0xb9, 0x08, 0x00, 0x40, 0xf9, 0x08, 0x35, 0x40, 0xf9, 0x00, 0x01, 0x3f, 0xd6, 0xe0, 0x08, 0x00, 0x35, 0x68, 0x76, 0x43, 0xb9};
inline constexpr uint8_t kCode2[] = {0x68, 0x86, 0x41, 0xf9, 0xa9, 0x43, 0x9f, 0xb8, 0x17, 0x01, 0x09, 0x8b, 0xa9, 0x03, 0x9f, 0xb8, 0xb6, 0x02, 0x40, 0xf9, 0x68, 0x7e, 0x43, 0xb9};
inline constexpr uint8_t kCode3[] = {0x69, 0x86, 0x41, 0xf9, 0xaa, 0xc3, 0x9e, 0xb8, 0x34, 0x01, 0x0a, 0x8b, 0xa9, 0x83, 0x9e, 0xb8, 0xb5, 0x06, 0x40, 0xf9, 0xdf, 0x02, 0x09, 0xeb};
inline constexpr uint8_t kCode4[] = {0x60, 0x9a, 0x41, 0xfd, 0x08, 0x90, 0xd0, 0xd2, 0xc8, 0x25, 0xe8, 0xf2, 0x01, 0x01, 0x67, 0x9e, 0x00, 0xd8, 0x61, 0x5e, 0x00, 0x18, 0x61, 0x1e, 0x00, 0x40, 0x62, 0x1e, 0x60, 0xa2, 0x03, 0xbd};
inline constexpr uint8_t kCode5[] = {0x68, 0x32, 0x0e, 0x91, 0xe9, 0x03, 0x00, 0xb2, 0x09, 0x01, 0x00, 0xf9, 0x68, 0x9a, 0x41, 0xf9, 0x68, 0xb6, 0x01, 0xf9, 0x48, 0x00, 0x80, 0x52, 0x9f, 0xfe, 0x5f, 0x88, 0x88, 0xfe, 0x09, 0x88, 0xc9, 0xff, 0xff, 0x35, 0x68, 0x5a, 0x43, 0xb9, 0x74, 0xe2, 0x0c, 0x91};

struct CodePart { uint64_t offset; const uint8_t* bytes; size_t size; };
inline constexpr CodePart kCodeParts[] = {
    {0xcd7888ull, kCode0, sizeof(kCode0)},
    {0xcd7998ull, kCode1, sizeof(kCode1)},
    {0xcd79d0ull, kCode2, sizeof(kCode2)},
    {0xcd7a48ull, kCode3, sizeof(kCode3)},
    {0xcd7aacull, kCode4, sizeof(kCode4)},
    {0xcd7018ull, kCode5, sizeof(kCode5)},
};

// Read must refuse unreadable ranges and copy only on success. Reject arithmetic
// overflow here independently of the implementation of Read.
template <class Read>
bool read_at(Read& read, uint64_t base, uint64_t offset, void* out, size_t size) {
    if (!base || offset > UINT64_MAX - base) return false;
    const uint64_t addr = base + offset;
    if (!size || size - 1 > UINT64_MAX - addr) return false;
    return read(addr, out, size);
}

template <class Read>
bool supported_layout(Read& read, uint64_t module_base) {
    for (const auto& part : kCodeParts) {
        uint8_t bytes[64];
        if (part.size > sizeof(bytes) ||
            !read_at(read, module_base, part.offset, bytes, part.size) ||
            std::memcmp(bytes, part.bytes, part.size) != 0) return false;
    }
    return true;
}

template <class Read>
Capture snapshot(Read& read, uint64_t controller, uint64_t self, uint64_t index, uint64_t planes) {
    Capture captured;
    uint32_t state = 0, selected = 0, open = 0;
    uint64_t decoder = 0;
    uint64_t plane_addresses[2] = {};
    if (!self ||
        !read_at(read, controller, 0x320, &state, sizeof(state)) || state != 3 ||
        !read_at(read, controller, 0x160, &decoder, sizeof(decoder)) || decoder != self ||
        !read_at(read, controller, 0x328, &selected, sizeof(selected)) || selected != uint32_t(index) ||
        !read_at(read, controller, 0x10, &open, sizeof(open)) || open != 1 ||
        !read_at(read, controller, 0x18, &captured.file_handle, sizeof(captured.file_handle)) ||
        !captured.file_handle ||
        !read_at(read, controller, 0x330, &captured.pts_us, sizeof(captured.pts_us)) || captured.pts_us < 0 ||
        !read_at(read, planes, 0, plane_addresses, sizeof(plane_addresses)) ||
        !plane_addresses[0] || !plane_addresses[1]) return {};
    captured.luma = plane_addresses[0];
    captured.chroma = plane_addresses[1];
    captured.valid = true;
    return captured;
}

// Clear even when the fingerprint or the snapshot is rejected. Other callers
// must not clear another pending frame on this thread.
template <class Read, class Publish>
Capture begin(bool matching, uint64_t module_base, uint64_t controller, uint64_t self,
              uint64_t index, uint64_t planes, Read& read, Publish& publish) {
    if (!matching) return {};
    publish(0, -1, 0, 0);
    if (!supported_layout(read, module_base)) return {};
    return snapshot(read, controller, self, index, planes);
}

template <class Publish>
void finish(const Capture& captured, bool real_called, uint64_t result, Publish& publish) {
    // The caller tests w0, not all of x0. A null slot is not a real success.
    if (captured.valid && real_called && uint32_t(result) == 0)
        publish(captured.file_handle, captured.pts_us, captured.luma, captured.chroma);
}

}  // namespace nx::movie_frame_source
