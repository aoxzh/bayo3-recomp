// BC7 encoding of RGBA8 texels (nvn_bc7_encode.cpp), for ASTC textures on GPUs that have BC but not ASTC
// (nvn_astc_bc7.cpp). Mode 6 only: one subset, RGBA endpoints of 7 bits plus a p-bit each, 4-bit indices.
#pragma once

#include <cstdint>

namespace nx::nvn {

// One 4x4 block: rgba is 16 texels, row by row, 4 bytes each. out: the 16-byte BC7 block.
void bc7_encode_block(const uint8_t* rgba, uint8_t* out);

// A width x height image of `slices` layers (tight RGBA8 rows, one layer after another) into BC7 blocks, row of
// blocks by row of blocks, layer after layer. Edge blocks repeat the last row and column.
void bc7_encode_image(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t slices, uint8_t* out);

// Bytes of BC7 for one layer of a width x height level.
inline uint64_t bc7_level_bytes(uint32_t width, uint32_t height) {
    return uint64_t((width + 3) / 4) * ((height + 3) / 4) * 16;
}

}  // namespace nx::nvn
