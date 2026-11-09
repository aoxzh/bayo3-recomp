// ASTC textures as BC7 on GPUs that have BC but not ASTC (Intel Arc, AMD: the Steam Deck, Radeon), through a disk
// cache (nvn_astc_bc7.cpp). Without it every ASTC texture is decoded on the CPU to RGBA8 at each load: in the
// Prologue on an Arc B390, 1,601 textures, 3.3 s of decoding in 110 s and up to 1.2 s of it in one 60-frame window,
// into 6.0 GB of RGBA8. BC7 is a quarter of that and needs no decoding.
//
// The first time a texture is seen it is decoded to RGBA8 as before, and a background thread at the lowest priority
// decodes it again, encodes BC7 (nvn_bc7_encode.cpp) and writes <data root>/texcache/<key>.bc7. The next time the
// texture's image is made, its key is in the cache: the image is BC7 and the upload reads the file. So nothing gets
// slower the first time, and later loads read a quarter of the bytes and decode nothing.
//
// NX_ASTC_BC7=0 turns it off (RGBA8 as before); NX_TEXCACHE_MB caps the cache (default 8192; the oldest files go).
#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_core.h>

namespace nx::nvn {

struct Texture;
struct GpuImage;
struct LinearLevel;

// At device creation: whether ASTC goes through BC7 here (no native ASTC, BC7 sampled and copied to).
void set_astc_bc7(bool supported);
bool astc_bc7_on();

// The cache key of a texture's current contents: its tiled bytes and its shape.
uint64_t astc_bc7_key(const Texture& t);
// Whether the cache holds that key (the index read from the folder, plus what this run wrote).
bool astc_bc7_cached(uint64_t key);
VkFormat astc_bc7_format(uint32_t nvn_format);  // BC7 UNORM or sRGB, as the ASTC format is

// The bytes of BC7 for these levels (`slices` layers each), laid out level after level.
uint64_t astc_bc7_size(const std::vector<LinearLevel>& levels, uint32_t slices);
// Read a cached texture into dst (astc_bc7_size bytes). False when the file is missing or does not match.
bool astc_bc7_read(uint64_t key, uint8_t* dst, uint64_t size);
// Decode the ASTC blocks and encode BC7 into dst now (an image made BC7 whose contents changed since).
void astc_bc7_encode_now(const uint8_t* blocks, const std::vector<LinearLevel>& levels, uint32_t slices, uint32_t bw,
                         uint32_t bh, bool srgb, uint8_t* dst);
// Queue the background encode of a texture this run uploaded as RGBA8 (a copy of its ASTC blocks is kept). Dropped
// when the key is cached or queued already, or past the queue's byte budget.
void astc_bc7_queue(uint64_t key, const std::vector<uint8_t>& blocks, const std::vector<LinearLevel>& levels,
                    uint32_t slices, uint32_t bw, uint32_t bh, bool srgb);
// Write BC7 bytes for a key (the synchronous path's result), in the background.
void astc_bc7_store(uint64_t key, std::vector<uint8_t> bc7);

}  // namespace nx::nvn
