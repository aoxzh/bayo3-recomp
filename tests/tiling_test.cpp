// Block-linear untiling test (runtime/src/nvn/nvn_tiling.cpp, docs/nvn/TILING.md). Standalone:
//   clang++ -std=c++20 -O2 -Iruntime/src/nvn tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp -o <out>
//
//   tiling_test --selftest
//       compares the fast GOB copies with a byte-by-byte reference (the GOB address formula applied to every byte)
//       over many sizes, block sizes, block heights and depths, then times a 1920x1080 RGBA8 level.
//   tiling_test <name.tiled.bin> <name.txt> <outdir>
//       untiles a texture extracted by `tools/maxwell/bntx.py --extract` (name.txt: width height layers levels
//       block_w block_h bytes_per_block block_height_gobs format, then the file's mip offsets), checks the mip
//       offsets against block_linear_layout(), and writes <outdir>/<name>.linear.bin (all levels, level-major) and
//       <outdir>/<name>.levels.txt (per level: offset layer_size width height width_blocks height_blocks) for
//       tools that decode compressed blocks, plus <outdir>/<name>.L<level>.ppm for 1x1-block 4-byte formats.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "nvn_tiling.h"

using namespace nx::nvn;

namespace {

// Reference: the address of every byte from the layout definition, no shortcuts.
uint64_t reference_address(uint64_t xb, uint32_t y, uint32_t z, uint64_t pitch, uint32_t height, uint32_t bh,
                           uint32_t bd, uint64_t padded_gobs = 0) {
    uint64_t gobs_x = padded_gobs ? padded_gobs : (pitch + 63) / 64;
    uint64_t block = 512ull * bh * bd;
    uint64_t rows_of_blocks = (height + 8 * bh - 1) / (8 * bh);
    uint64_t gx = xb / 64, gy = y / 8;
    uint64_t addr = (z / bd) * rows_of_blocks * gobs_x * block;     // slab of block_depth slices
    addr += (gy / bh) * gobs_x * block + gx * block;                // block
    addr += ((z % bd) * bh + gy % bh) * 512;                        // GOB inside the block
    uint64_t x = xb % 64, yy = y % 8;
    addr += (x / 32) * 256 + (yy / 2) * 64 + ((x / 16) % 2) * 32 + (yy % 2) * 16 + x % 16;
    return addr;
}

bool selftest() {
    std::mt19937 rng(1234);
    const uint32_t bpbs[] = {1, 2, 4, 8, 12, 16};
    int cases = 0, failures = 0;
    for (int iter = 0; iter < 3000; iter++) {
        uint32_t bpb = bpbs[rng() % 6];
        uint32_t w = 1 + rng() % (iter < 1500 ? 40 : 300), h = 1 + rng() % (iter < 1500 ? 40 : 300);
        uint32_t d = iter % 4 == 0 ? 1 + rng() % 9 : 1;
        uint32_t bh = 1u << (rng() % 6), bd = d > 1 ? 1u << (rng() % 4) : 1;
        uint64_t size = block_linear_size(w, h, d, bpb, bh, bd);
        std::vector<uint8_t> src(size), got(uint64_t(w) * bpb * h * d, 0xCD), want(got.size());
        for (auto& b : src) b = uint8_t(rng());
        uint64_t pitch = uint64_t(w) * bpb;
        for (uint32_t z = 0; z < d; z++)
            for (uint32_t y = 0; y < h; y++)
                for (uint64_t x = 0; x < pitch; x++) {
                    uint64_t a = reference_address(x, y, z, pitch, h, bh, bd);
                    if (a >= size) {
                        std::printf("reference address out of range: w=%u h=%u\n", w, h);
                        return false;
                    }
                    want[(uint64_t(z) * h + y) * pitch + x] = src[a];
                }
        deswizzle_block_linear(src.data(), got.data(), w, h, d, bpb, bh, bd);
        cases++;
        if (got != want) {
            if (failures++ < 10) std::printf("MISMATCH w=%u h=%u d=%u bpb=%u bh=%u bd=%u\n", w, h, d, bpb, bh, bd);
        }
    }
    std::printf("selftest: %d cases, %d failures\n", cases, failures);

    // Whole-texture walk: a 2D array with mips, each (level, layer) against a direct call.
    {
        uint32_t w = 300, h = 170, layers = 3, levels = 9;
        TextureLayout t = block_linear_layout(w, h, layers, levels, 4, 4, 16, 8);
        std::vector<uint8_t> tiled(t.tiled_size), lin, one;
        for (auto& b : tiled) b = uint8_t(rng());
        auto lv = deswizzle_texture(tiled.data(), tiled.size(), w, h, layers, levels, 4, 4, 16, lin, 8);
        bool ok = lv.size() == levels;
        for (uint32_t l = 0; ok && l < levels; l++)
            for (uint32_t a = 0; a < layers; a++) {
                one.assign(lv[l].layer_size, 0);
                deswizzle_block_linear(tiled.data() + a * t.tiled_layer_stride + lv[l].tiled_offset, one.data(),
                                       lv[l].width_blocks, lv[l].height_blocks, 1, 16, lv[l].block_height_gobs, 1);
                ok &= std::memcmp(one.data(), lin.data() + lv[l].offset + a * lv[l].layer_size, one.size()) == 0;
                ok &= lv[l].offset % 16 == 0;
            }
        ok &= deswizzle_texture(tiled.data(), tiled.size() - 1, w, h, layers, levels, 4, 4, 16, lin, 8).empty();
        std::printf("texture walk: %s\n", ok ? "ok" : "FAILED");
        failures += !ok;
    }

    // Original B3 SDK 128^3 RGBA8 packaged volume: height1/depth16,
    // independently measured mip sizes/offsets (not computed by this helper).
    {
        const uint64_t offsets[] = {0,0x800000,0x900000,0x920000,0x924000,0x925000,0x925800,0x925c00};
        const uint64_t sizes[] = {0x800000,0x100000,0x20000,0x4000,0x1000,0x800,0x400,0x200};
        const uint32_t depths[] = {16,16,16,16,8,4,2,1};
        auto volume=block_linear_volume_layout(128,128,128,8,1,1,4,1,16);
        bool ok=volume.tiled_size==0x925e00 && volume.levels.size()==8;
        std::vector<uint8_t> tiled(volume.tiled_size), linear;
        for(auto& b:tiled)b=uint8_t(rng());
        auto levels=deswizzle_volume(tiled.data(),tiled.size(),128,128,128,8,1,1,4,linear,1,16);
        ok &= levels.size()==8;
        for(size_t k=0;ok&&k<levels.size();++k){
            const auto& l=levels[k];
            ok &= l.tiled_offset==offsets[k] && l.block_depth_gobs==depths[k] && l.depth==(128u>>k);
            ok &= block_linear_size(l.width,l.height,l.depth,4,1,l.block_depth_gobs)==sizes[k];
            const uint64_t pitch=uint64_t(l.width)*4;
            for(uint32_t z=0;ok&&z<l.depth;++z)
                for(uint32_t y=0;ok&&y<l.height;++y)
                    for(uint64_t x=0;x<pitch;++x)
                        if(linear[l.offset+(uint64_t(z)*l.height+y)*pitch+x] !=
                           tiled[offsets[k]+reference_address(x,y,z,pitch,l.height,1,depths[k])]) {ok=false;break;}
        }
        ok &= deswizzle_volume(tiled.data(),tiled.size()-1,128,128,128,8,1,1,4,linear,1,16).empty();
        std::printf("B3 true-volume mip walk: %s\n",ok?"ok":"FAILED");failures+=!ok;
    }

    // Padded source rows: 300x1024 ASTC4x4 has 19 natural GOBs,
    // padded to32 while height drops below baseH8 on later mips. Arrays must
    // preserve layer offsets and omit all source padding from linear output.
    {
        auto layout=block_linear_layout(300,1024,2,5,4,4,16,8,16);
        bool ok=layout.levels.size()==5 && layout.levels[0].tiled_pitch_gobs==32;
        std::vector<uint8_t> source(layout.tiled_size),linear;
        for(auto& b:source)b=uint8_t(rng());
        auto levels=deswizzle_texture(source.data(),source.size(),300,1024,2,5,4,4,16,linear,8,16);
        ok &= levels.size()==5;
        for(const auto& l:levels){
            const uint64_t pitch=uint64_t(l.width_blocks)*16;
            for(uint32_t layer=0;ok&&layer<2;++layer)
                for(uint32_t y=0;ok&&y<l.height_blocks;++y)
                    for(uint64_t x=0;x<pitch;++x){
                        const uint64_t a=layer*layout.tiled_layer_stride+l.tiled_offset+
                            reference_address(x,y,0,pitch,l.height_blocks,l.block_height_gobs,1,l.tiled_pitch_gobs);
                        if(linear[l.offset+layer*l.layer_size+y*pitch+x]!=source[a]){ok=false;break;}
                    }
        }
        auto inactive=block_linear_layout(300,128,1,1,4,4,16,8,16);
        ok &= inactive.levels[0].tiled_pitch_gobs==19;
        auto observed=block_linear_layout(1026,1024,1,1,6,5,16,8,16);
        ok &= observed.levels[0].tiled_pitch_gobs==48;
        // The same padded block-row stride must apply across volume slabs.
        auto volume=block_linear_volume_layout(300,64,4,3,1,1,4,2,4,16);
        std::vector<uint8_t> tiled(volume.tiled_size),out;
        for(auto& b:tiled)b=uint8_t(rng());
        auto vl=deswizzle_volume(tiled.data(),tiled.size(),300,64,4,3,1,1,4,out,2,4,16);
        ok &= !vl.empty() && vl[0].tiled_pitch_gobs==32;
        for(const auto& l:vl){
            const uint64_t pitch=uint64_t(l.width)*4;
            for(uint32_t z=0;ok&&z<l.depth;++z)
                for(uint32_t y=0;ok&&y<l.height;++y)
                    for(uint64_t x=0;x<pitch;++x)
                        if(out[l.offset+(uint64_t(z)*l.height+y)*pitch+x] != tiled[l.tiled_offset+
                           reference_address(x,y,z,pitch,l.height,l.block_height_gobs,l.block_depth_gobs,l.tiled_pitch_gobs)]){ok=false;break;}
        }
        ok &= deswizzle_texture(source.data(),source.size()-1,300,1024,2,5,4,4,16,linear,8,16).empty();
        std::printf("padded source rows/arrays/volumes: %s\n",ok?"ok":"FAILED");failures+=!ok;
    }

    // Speed: a 1920x1080 RGBA8 level (block height 16), the size of a full-screen UI texture.
    {
        uint32_t w = 1920, h = 1080;
        std::vector<uint8_t> src(block_linear_size(w, h, 1, 4, 16, 1), 7), dst(uint64_t(w) * h * 4);
        auto t0 = std::chrono::steady_clock::now();
        const int n = 50;
        for (int i = 0; i < n; i++) deswizzle_block_linear(src.data(), dst.data(), w, h, 1, 4, 16, 1);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::printf("1920x1080 RGBA8: %.3f ms per level (%.1f GB/s)\n", ms, dst.size() / ms / 1e6);
    }
    return failures == 0;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int run(const std::string& tiled_path, const std::string& params_path, const std::string& outdir) {
    std::vector<uint8_t> tiled;
    if (!read_file(tiled_path, tiled)) {
        std::fprintf(stderr, "cannot read %s\n", tiled_path.c_str());
        return 1;
    }
    std::ifstream p(params_path);
    uint32_t w, h, layers, levels, bw, bhb, bpb, bh0;
    std::string format;
    if (!(p >> w >> h >> layers >> levels >> bw >> bhb >> bpb >> bh0 >> format)) {
        std::fprintf(stderr, "bad parameter file %s\n", params_path.c_str());
        return 1;
    }
    std::vector<uint64_t> file_mips(levels);
    for (auto& m : file_mips) p >> m;

    TextureLayout t = block_linear_layout(w, h, layers, levels, bw, bhb, bpb, bh0);
    bool mips_ok = true;
    for (uint32_t l = 0; l < levels; l++) mips_ok &= t.levels[l].tiled_offset == file_mips[l];
    std::printf("%s %ux%u layers=%u levels=%u block %ux%u/%uB bh=%u: tiled %" PRIu64 " bytes (file %zu), mip "
                "offsets %s\n",
                format.c_str(), w, h, layers, levels, bw, bhb, bpb, bh0, t.tiled_size, tiled.size(),
                mips_ok ? "match" : "DIFFER");

    std::vector<uint8_t> linear;
    auto t0 = std::chrono::steady_clock::now();
    auto lv = deswizzle_texture(tiled.data(), tiled.size(), w, h, layers, levels, bw, bhb, bpb, linear, bh0);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (lv.empty()) {
        std::fprintf(stderr, "deswizzle_texture refused the parameters\n");
        return 1;
    }
    std::printf("untiled %zu bytes in %.3f ms\n", linear.size(), ms);

    std::string base = tiled_path.substr(tiled_path.find_last_of("/\\") + 1);  // either separator (Windows)
    base = base.substr(0, base.find(".tiled.bin"));
    std::ofstream(outdir + "/" + base + ".linear.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(linear.data()), std::streamsize(linear.size()));
    std::ofstream lt(outdir + "/" + base + ".levels.txt");
    for (const auto& l : lv)
        lt << l.offset << ' ' << l.layer_size << ' ' << l.width << ' ' << l.height << ' ' << l.width_blocks << ' '
           << l.height_blocks << '\n';
    if (bw == 1 && bhb == 1 && bpb == 4) {  // RGBA8-like: a PPM of layer 0 of every level (alpha dropped)
        for (uint32_t l = 0; l < lv.size(); l++) {
            std::ofstream ppm(outdir + "/" + base + ".L" + std::to_string(l) + ".ppm", std::ios::binary);
            ppm << "P6\n" << lv[l].width << ' ' << lv[l].height << "\n255\n";
            const uint8_t* px = linear.data() + lv[l].offset;
            for (uint64_t i = 0; i < uint64_t(lv[l].width) * lv[l].height; i++) ppm.write((const char*)px + i * 4, 3);
        }
    }
    return mips_ok ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--selftest") return selftest() ? 0 : 1;
    if (argc == 4) return run(argv[1], argv[2], argv[3]);
    std::fprintf(stderr, "usage: tiling_test --selftest | <name.tiled.bin> <name.txt> <outdir>\n");
    return 1;
}
