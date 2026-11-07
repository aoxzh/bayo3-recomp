// NVN fixed-function alpha compare, specialized with the pipeline's reference.
// Wrap the translated entry rather than inserting control flow inside its CFG:
// every normal return is tested and an existing discard still terminates directly.
#include "nvn_shaders.h"
#include <cstring>
#include <map>

namespace nx::nvn {
bool patch_alpha_test(const std::vector<uint32_t>& in, uint32_t compare, float reference,
                      std::vector<uint32_t>& out) {
    if (compare < 1 || compare > 8 || in.size() < 5 || in[0] != 0x07230203u) return false;
    if (compare == 8) { out = in; return true; }
    struct Inst { size_t at; uint32_t op, len; };
    std::vector<Inst> insts;
    std::map<uint32_t, std::vector<uint32_t>> types;
    std::map<uint32_t, uint32_t> locations, components, indices;
    uint32_t entry = 0, void_type = 0, bool_type = 0, float_type = 0, function_type = 0;
    size_t first_function = in.size();
    for (size_t i = 5; i < in.size();) {
        uint32_t n = in[i] >> 16, op = in[i] & 0xffff;
        if (!n || n > in.size() - i) return false;
        const auto* w = &in[i];
        insts.push_back({i, op, n});
        if (op == 15) {
            if (n < 4 || w[1] != 4 || entry) return false; // exactly one fragment entry
            entry = w[2];
        }
        // EarlyFragmentTests would commit depth/stencil before our discard.
        // Such a module needs translator-level handling, not this late test.
        if (op == 16 && n >= 3 && w[2] == 9) return false;
        if (op >= 19 && op <= 33 && n >= 2) types[w[1]] = {w, w + n};
        if (op == 19 && n == 2) void_type = w[1];
        if (op == 20 && n == 2) bool_type = w[1];
        if (op == 71 && n == 4) {
            if (w[2] == 30) locations[w[1]] = w[3];
            if (w[2] == 31) components[w[1]] = w[3];
            if (w[2] == 32) indices[w[1]] = w[3];
        }
        if (op == 54 && first_function == in.size()) first_function = i;
        i += n;
    }
    if (!entry || !void_type || first_function == in.size()) return false;
    uint32_t output = 0, output_type = 0;
    for (const auto& inst : insts) {
        const auto* w = &in[inst.at];
        if (inst.op == 54 && inst.len == 5 && w[2] == entry) {
            if (w[1] != void_type) return false;
            function_type = w[4];
        }
        if (inst.op != 59 || inst.len < 4 || w[3] != 3 ||
            !locations.count(w[2]) || locations[w[2]] != 0 || indices[w[2]] != 0) continue;
        if (output || components[w[2]] != 0) return false;
        auto ptr = types.find(w[1]);
        if (ptr == types.end() || ptr->second.size() != 4 || (ptr->second[0] & 0xffff) != 32) return false;
        auto vec = types.find(ptr->second[3]);
        if (vec == types.end() || vec->second.size() != 4 || (vec->second[0] & 0xffff) != 23 || vec->second[3] != 4)
            return false;
        auto scalar = types.find(vec->second[2]);
        if (scalar == types.end() || scalar->second.size() != 3 || (scalar->second[0] & 0xffff) != 22 || scalar->second[2] != 32)
            return false;
        output = w[2]; output_type = ptr->second[3]; float_type = vec->second[2];
    }
    auto ft = types.find(function_type);
    if (ft == types.end() || ft->second.size() != 3 || (ft->second[0] & 0xffff) != 33) return false;
    // Ryujinx only compares when color0 alpha is exported. NEVER discards even
    // depth-only fragments; no color0 with other compares retains the original.
    if (!output && compare != 1) { out = in; return true; }
    uint32_t bound = in[3];
    if (!bound || bound > 0xfffffff0u) return false;
    uint32_t wrapper = bound++, label = bound++, called = bound++;
    std::vector<uint32_t> globals;
    uint32_t ref = 0;
    if (compare != 1) {
        if (!bool_type) {
            bool_type = bound++;
            globals.insert(globals.end(), {(2u << 16) | 20, bool_type});
        }
        uint32_t bits; std::memcpy(&bits, &reference, sizeof bits);
        ref = bound++;
        globals.insert(globals.end(), {(4u << 16) | 43, float_type, ref, bits});
    }
    out.assign(in.begin(), in.begin() + 5);
    for (const auto& inst : insts) {
        if (inst.at == first_function) out.insert(out.end(), globals.begin(), globals.end());
        size_t at = out.size();
        out.insert(out.end(), in.begin() + inst.at, in.begin() + inst.at + inst.len);
        if (inst.op == 15) out[at + 2] = wrapper;
        if ((inst.op == 16 || inst.op == 331) && inst.len >= 3 && out[at + 1] == entry) out[at + 1] = wrapper;
    }
    auto emit = [&](std::initializer_list<uint32_t> w) { out.insert(out.end(), w); };
    emit({(5u << 16) | 54, void_type, wrapper, 0, function_type});
    emit({(2u << 16) | 248, label});
    // NEVER still executes original shader side effects up to its normal end.
    emit({(4u << 16) | 57, void_type, called, entry});
    if (compare == 1) emit({(1u << 16) | 252});
    else {
        uint32_t value = bound++, alpha = bound++, pass = bound++, kill = bound++, merge = bound++;
        emit({(4u << 16) | 61, output_type, value, output});
        emit({(5u << 16) | 81, float_type, alpha, value, 3});
        // The translator uses ordered FP32 comparisons for all six operations,
        // including FOrdNotEqual: an unordered (NaN) comparison fails the test.
        static constexpr uint32_t ops[] = {0, 0, 184, 180, 188, 186, 182, 190};
        emit({(5u << 16) | ops[compare], bool_type, pass, alpha, ref});
        emit({(3u << 16) | 247, merge, 0});
        emit({(4u << 16) | 250, pass, merge, kill});
        emit({(2u << 16) | 248, kill});
        emit({(1u << 16) | 252});
        emit({(2u << 16) | 248, merge});
        emit({(1u << 16) | 253});
    }
    emit({(1u << 16) | 56});
    out[3] = bound;
    return true;
}
} // namespace nx::nvn
