// Optional position-arithmetic precision for translated Maxwell shaders.
#pragma once
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace nx::nvn {
// False means malformed input; code is unchanged. True also includes already precise modules.
// This checks the structure needed by the pass, not all SPIR-V semantic rules.
inline bool precise_position(std::vector<uint32_t>& code) {
    if (code.size() < 5 || code[0] != 0x07230203 || !code[3]) return false;
    std::unordered_set<uint32_t> glsl, decorated;
    size_t insert = 0;
    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (!wc || wc > code.size() - i) return false;
        if (!insert && op >= 19 && op <= 39) insert = i;
        if (op == 11) { // OpExtInstImport: only the actual GLSL.std.450 instruction set has Fma=50.
            if (wc < 3 || !code[i + 1] || code[i + 1] >= code[3]) return false;
            const char* name = reinterpret_cast<const char*>(&code[i + 2]);
            const size_t bytes = (wc - 2) * sizeof(uint32_t);
            const void* end = std::memchr(name, 0, bytes);
            if (!end) return false;
            if (static_cast<const char*>(end) - name == 12 && std::memcmp(name, "GLSL.std.450", 12) == 0)
                glsl.insert(code[i + 1]);
        }
        if (op == 71) {
            if (wc < 3) return false;
            if (code[i + 2] == 42) decorated.insert(code[i + 1]);
        }
        i += wc;
    }
    std::vector<uint32_t> annotations;
    for (size_t i = 5; i < code.size();) {
        const uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        uint32_t expected = 0;
        switch (op) {
            case 127: expected = 4; break; // FNegate
            case 129: case 131: case 133: case 136: case 140: case 141: // floating binary ops
            case 142: case 143: case 144: case 145: case 146: case 147: case 148:
                expected = 5; break; // vector/matrix products, outer product, dot
            case 12:
                if (wc < 5) return false;
                if (glsl.count(code[i + 3]) && code[i + 4] == 50) expected = 8;
                break;
        }
        if (expected) {
            if (wc != expected || !code[i + 2] || code[i + 2] >= code[3]) return false;
            if (decorated.insert(code[i + 2]).second) {
                annotations.insert(annotations.end(), {(3u << 16) | 71u, code[i + 2], 42u});
            }
        }
        i += wc;
    }
    if (!annotations.empty()) {
        if (!insert) return false;
        code.insert(code.begin() + insert, annotations.begin(), annotations.end());
    }
    return true;
}
} // namespace nx::nvn
