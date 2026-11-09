// BC7 mode 6 encoder (runtime/src/nvn/nvn_bc7_encode.cpp) against a mode 6 decoder written here from the format
// description: bit layout, the anchor rule, exact flat blocks, and the error on smooth, two-colour and noisy blocks.
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "nvn_bc7_encode.h"

namespace {

constexpr int kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

uint32_t bits(const uint8_t* b, int& pos, int n) {
    uint32_t v = 0;
    for (int k = 0; k < n; k++, pos++) v |= uint32_t(b[pos >> 3] >> (pos & 7) & 1) << k;
    return v;
}

void decode6(const uint8_t* b, uint8_t out[64]) {
    int pos = 0;
    assert(bits(b, pos, 7) == 64);  // mode 6
    int q[2][4];
    for (int c = 0; c < 4; c++) q[0][c] = int(bits(b, pos, 7)), q[1][c] = int(bits(b, pos, 7));
    int p0 = int(bits(b, pos, 1)), p1 = int(bits(b, pos, 1));
    int idx[16];
    idx[0] = int(bits(b, pos, 3));
    for (int t = 1; t < 16; t++) idx[t] = int(bits(b, pos, 4));
    assert(pos == 128);
    for (int t = 0; t < 16; t++)
        for (int c = 0; c < 4; c++) {
            int a = q[0][c] << 1 | p0, e = q[1][c] << 1 | p1;
            out[t * 4 + c] = uint8_t(((64 - kWeights[idx[t]]) * a + kWeights[idx[t]] * e + 32) >> 6);
        }
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double se = 0;
    for (size_t i = 0; i < a.size(); i++) se += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
    double mse = se / double(a.size());
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

double roundtrip(const std::vector<uint8_t>& block) {
    uint8_t enc[16], dec[64];
    nx::nvn::bc7_encode_block(block.data(), enc);
    decode6(enc, dec);
    return psnr(block, std::vector<uint8_t>(dec, dec + 64));
}

}  // namespace

int main() {
    std::mt19937 rng(7);
    // Flat blocks come back within the 8-bit rounding of a 7-bit endpoint plus p-bit: exact or off by one.
    for (int k = 0; k < 200; k++) {
        std::vector<uint8_t> b(64);
        uint8_t c[4] = {uint8_t(rng()), uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
        for (int t = 0; t < 16; t++)
            for (int i = 0; i < 4; i++) b[t * 4 + i] = c[i];
        uint8_t enc[16], dec[64];
        nx::nvn::bc7_encode_block(b.data(), enc);
        decode6(enc, dec);
        for (int i = 0; i < 64; i++) assert(std::abs(int(dec[i]) - int(b[i])) <= 1);
    }
    // Two colours (text, masks): close to exact.
    double worst2 = 99;
    for (int k = 0; k < 500; k++) {
        std::vector<uint8_t> b(64);
        uint8_t c[2][4];
        for (auto& col : c)
            for (auto& v : col) v = uint8_t(rng());
        for (int t = 0; t < 16; t++) {
            const int pick = int(rng() & 1);  // one of the two colours per texel
            for (int i = 0; i < 4; i++) b[t * 4 + i] = c[pick][i];
        }
        worst2 = std::min(worst2, roundtrip(b));
    }
    // Smooth gradients (most of a game texture).
    double worst_grad = 99;
    for (int k = 0; k < 500; k++) {
        std::vector<uint8_t> b(64);
        int base[4], dx[4], dy[4];
        for (int i = 0; i < 4; i++) base[i] = int(rng() % 200), dx[i] = int(rng() % 13) - 6, dy[i] = int(rng() % 13) - 6;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                for (int i = 0; i < 4; i++)
                    b[(y * 4 + x) * 4 + i] = uint8_t(std::clamp(base[i] + dx[i] * x + dy[i] * y, 0, 255));
        worst_grad = std::min(worst_grad, roundtrip(b));
    }
    // Noise: the worst case for one subset; it must still be a valid block that decodes to something near.
    double mean_noise = 0;
    for (int k = 0; k < 500; k++) {
        std::vector<uint8_t> b(64);
        for (auto& v : b) v = uint8_t(rng());
        mean_noise += roundtrip(b) / 500;
    }
    // A whole image with an odd size: the edge blocks repeat the last texel, and the block count is right.
    const uint32_t w = 37, h = 21;
    std::vector<uint8_t> img(w * h * 4 * 2);
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int i = 0; i < 4; i++) img[((s * h + y) * w + x) * 4 + i] = uint8_t(x * 6 + y * 3 + i * 40 + s * 9);
    std::vector<uint8_t> enc(nx::nvn::bc7_level_bytes(w, h) * 2);
    nx::nvn::bc7_encode_image(img.data(), w, h, 2, enc.data());
    std::vector<uint8_t> back(img.size());
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    for (uint32_t s = 0; s < 2; s++)
        for (uint32_t y = 0; y < by; y++)
            for (uint32_t x = 0; x < bx; x++) {
                uint8_t dec[64];
                decode6(enc.data() + ((s * by + y) * bx + x) * 16, dec);
                for (uint32_t j = 0; j < 4; j++)
                    for (uint32_t i = 0; i < 4; i++)
                        if (x * 4 + i < w && y * 4 + j < h)
                            for (int c = 0; c < 4; c++)
                                back[((s * h + y * 4 + j) * w + x * 4 + i) * 4 + c] = dec[(j * 4 + i) * 4 + c];
            }
    const double image = psnr(img, back);
    // Speed (informational): blocks a second on one thread.
    std::vector<uint8_t> big(512 * 512 * 4);
    for (auto& v : big) v = uint8_t(rng() % 64 + 96);
    std::vector<uint8_t> bigenc(nx::nvn::bc7_level_bytes(512, 512));
    auto t0 = std::chrono::steady_clock::now();
    nx::nvn::bc7_encode_image(big.data(), 512, 512, 1, bigenc.data());
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("bc7: two colours worst %.1f dB, gradients worst %.1f dB, noise mean %.1f dB, image %.1f dB, "
                "%.2f M blocks/s on one thread\n", worst2, worst_grad, mean_noise, image, 16384 / sec / 1e6);
    assert(worst2 > 30.0);
    assert(worst_grad > 30.0);  // one subset: a plane in four channels is not a line
    assert(mean_noise > 12.0);
    assert(image > 36.0);
    std::printf("PASS bc7 mode 6 encoder\n");
    return 0;
}
