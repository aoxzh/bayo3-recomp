// Vertex inputs with integer formats (docs/archive/nvn/DEBUG.md #20). The offline translator declares every vertex input as a
// float vec4 (TRANSLATOR.md, "What only the runtime knows"): on Maxwell an attribute of an integer format arrives in
// the shader as the integer's bits, and the code reinterprets them (skinning: `floatBitsToInt(blend_index) * 48`).
// With a float input, Vulkan/Metal convert the integer to a float value (3 -> 3.0 = 0x40400000), so every bone index
// was garbage and skinned models (every character) were drawn with matrices read from past the palette.
//
// patch_int_inputs() rewrites a module so the chosen locations are declared as ivec4/uvec4 and every load is bitcast
// back to the float the rest of the code expects: the Maxwell semantics exactly.
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpFunction = 54, OpVariable = 59,
    OpLoad = 61, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpBitcast = 124,
    DecorationLocation = 30, StorageInput = 1,
};

struct Inst {
    size_t at;  // word index
    uint32_t op, len;
};

// True if the instruction at `w` passes pointer `id` to something other than a load or an access chain: OpStore,
// OpCopyMemory, OpFunctionCall, OpCopyObject, OpPtrAccessChain, OpExtInst (InterpolateAt*); with `load`, OpLoad too.
// Only operand positions that hold ids are compared (a literal, such as an OpExtInst number, can equal the id).
bool other_pointer_use(const uint32_t* w, uint32_t op, uint32_t len, uint32_t id, bool load) {
    auto any = [&](uint32_t from, uint32_t to) {
        for (uint32_t k = from; k < std::min(to, len); k++)
            if (w[k] == id) return true;
        return false;
    };
    switch (op) {
        case 61: return load && any(3, 4);                    // OpLoad: the pointer
        case 62: case 63: return any(1, 3);                   // OpStore, OpCopyMemory: the two operands
        case 57: case 12: return any(op == 57 ? 4 : 5, len);  // OpFunctionCall, OpExtInst: the arguments
        case 83: case 67: return any(3, len);                 // OpCopyObject, OpPtrAccessChain
        default: return false;
    }
}

}  // namespace

// 0 float (as is), 1 unsigned integer bits, 2 signed integer bits.
uint32_t vertex_int_kind(uint32_t nvn_format) {
    switch (vk_vertex_format(nvn_format)) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT:
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return 1;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT:
        case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
            return 2;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT:
            return nvn_format >= 0x55 && nvn_format <= 0x68 ? 0 : 1;  // 32-bit "scaled" wants the value: keep float
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            return nvn_format >= 0x55 && nvn_format <= 0x68 ? 0 : 2;
        default: return 0;
    }
}

bool patch_int_inputs(const std::vector<uint32_t>& in, const uint8_t kinds[16], std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    std::map<uint32_t, uint32_t> location;               // id -> Location
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ints;  // (width, signed) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs;  // (component, count) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;  // (storage, pointee) -> id
    std::map<uint32_t, size_t> defined_at;               // type id -> word index
    uint32_t f32 = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4) ints[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        if (n.op >= OpTypeInt && n.op <= OpTypePointer) defined_at[w[1]] = n.at;
    }
    if (!f32) return false;
    uint32_t v4f = vecs.count({f32, 4}) ? vecs[{f32, 4}] : 0;
    uint32_t ptr_v4f = v4f && ptrs.count({StorageInput, v4f}) ? ptrs[{StorageInput, v4f}] : 0;
    if (!ptr_v4f) return false;

    // The input variables to change, and the types they need (added before the first function when missing).
    std::map<uint32_t, uint32_t> patched;  // variable or access chain id -> signedness (0 unsigned, 1 signed)
    size_t insert_at = in.size();          // the new types go right before the first patched variable
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageInput || w[1] != ptr_v4f) continue;
        auto l = location.find(w[2]);
        if (l == location.end() || l->second >= 16 || !kinds[l->second]) continue;
        patched[w[2]] = kinds[l->second] == 2 ? 1 : 0;
        insert_at = std::min(insert_at, n.at);
    }
    if (patched.empty()) return false;
    std::vector<uint32_t> types;
    struct Types { uint32_t scalar, vec4, ptr_scalar, ptr_vec4; } t[2] = {};
    bool ok = true;  // false if an existing type we reuse is declared after the insertion point
    for (uint32_t s = 0; s < 2; s++) {
        bool used = false;
        for (auto& p : patched) used |= p.second == s;
        if (!used) continue;
        auto get = [&](auto& table, std::pair<uint32_t, uint32_t> key, uint32_t op, uint32_t a, uint32_t b) {
            auto it = table.find(key);
            if (it != table.end()) {
                if (defined_at.count(it->second) && defined_at[it->second] > insert_at) ok = false;
                return it->second;
            }
            uint32_t id = bound++;
            types.insert(types.end(), {(4u << 16) | op, id, a, b});
            table[key] = id;
            return id;
        };
        t[s].scalar = get(ints, {32, s}, OpTypeInt, 32, s);
        t[s].vec4 = get(vecs, {t[s].scalar, 4}, OpTypeVector, t[s].scalar, 4);
        t[s].ptr_scalar = get(ptrs, {StorageInput, t[s].scalar}, OpTypePointer, StorageInput, t[s].scalar);
        t[s].ptr_vec4 = get(ptrs, {StorageInput, t[s].vec4}, OpTypePointer, StorageInput, t[s].vec4);
    }

    if (!ok) return false;
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == insert_at) out.insert(out.end(), types.begin(), types.end());
        std::vector<uint32_t> w(in.begin() + n.at, in.begin() + n.at + n.len);
        if (n.op == OpVariable && patched.count(w[2])) {
            w[1] = t[patched[w[2]]].ptr_vec4;
        } else if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 4 && patched.count(w[3])) {
            uint32_t s = patched[w[3]];
            if (n.len == 4) return false;  // a chain with no index: not produced by the translator
            w[1] = t[s].ptr_scalar;  // one index into a vec4: a scalar
            patched[w[2]] = s;
        } else if (n.op == OpLoad && n.len >= 4 && patched.count(w[3])) {
            uint32_t s = patched[w[3]], type = w[1], result = w[2], loaded = bound++;
            w[1] = type == v4f ? t[s].vec4 : t[s].scalar;
            w[2] = loaded;
            out.insert(out.end(), w.begin(), w.end());
            out.insert(out.end(), {(4u << 16) | OpBitcast, type, result, loaded});
            continue;
        } else if (n.op == 62 || n.op == 63 || n.op == 57 || n.op == 83 || n.op == 67) {
            // OpStore, OpCopyMemory, OpFunctionCall, OpCopyObject, OpPtrAccessChain on a patched pointer would keep
            // the float type (the translator emits none of them for inputs): leave this module as it is.
            for (uint32_t k = 1; k < n.len; k++)
                if (patched.count(w[k])) return false;
        }
        out.insert(out.end(), w.begin(), w.end());
    }
    out[3] = bound;
    return true;
}

// ---- constant buffer reads past the bound size ------------------------------------------------------------------
// On Maxwell a constant buffer read past the bound size returns 0. The translator declares every bank as vec4[4096] and
// Metal checks nothing, so a computed index past the data reads whatever follows in the upload buffer. Wonder's lit
// materials (uber/agl fragment shaders) loop over a screen tile's light list in c[8]: the tile comes from gl_FragCoord
// scaled for a 384x216 grid (c[6][62].zw = 1/384, 1/216), so a 1920x1080 pass computes tiles up to ~800 while the bound
// list (0xb000 bytes) holds 144. The game relies on those reads returning 0 (no lights); here they returned garbage
// light counts, the loop ran for billions of iterations and Metal killed the command buffer (GPU hang ->
// VK_ERROR_DEVICE_LOST): the picture froze at the first talking-flower balloon of the prologue course while the game
// (and its sound) ran on (docs/archive/nvn/DEBUG.md #21). Fix: the array index of such reads is clamped to 4095 (UMin: negative
// indices clamp too) and those bindings are bound zero-padded to 64 KB, so every read past the data returns 0.
bool clamp_uniform_indices(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint64_t* bindings) {
    enum : uint32_t {
        OpExtInstImport = 11, OpExtInst = 12, OpConstant = 43, OpStore = 62, OpDecorate = 71, OpLabel = 248,
        DecorationBinding = 33, DecorationDescriptorSet = 34, StorageUniform = 2, GlslUMin = 38,
    };
    if (bindings) *bindings = 0;
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t glsl = 0, bound = in[3];
    std::map<uint32_t, uint32_t> binding, set, type_of;  // id -> Binding, DescriptorSet, value type
    std::map<uint32_t, uint32_t> ints;                    // int type id -> its 4095 constant (0: none yet)
    std::map<uint32_t, bool> uniform_var, constant;
    size_t first_function = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpExtInstImport && n.len >= 3 &&
            std::strcmp(reinterpret_cast<const char*>(w + 2), "GLSL.std.450") == 0)
            glsl = w[1];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBinding) binding[w[1]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationDescriptorSet) set[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32) ints.emplace(w[1], 0);
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageUniform) uniform_var[w[2]] = true;
        else if (n.op == OpConstant && n.len >= 4) {
            constant[w[2]] = true;
            if (ints.count(w[1]) && w[3] == 4095) ints[w[1]] = w[2];
        } else if (n.op == OpFunction) first_function = std::min(first_function, n.at);
        // Values inside functions: <op> <type> <id> (the few instructions without a result are skipped).
        if (n.at > first_function && n.len >= 3 && n.op != OpStore && n.op != OpDecorate && n.op != OpLabel &&
            n.op != OpFunction && (n.op < 246 || n.op > 256))
            type_of.emplace(w[2], w[1]);
    }
    if (!glsl) return false;
    // The chains to clamp: base a set 0 uniform variable, then member 0, then a computed array index.
    std::map<size_t, uint32_t> clamp;  // word index of the chain -> index type
    uint64_t mask = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op != OpAccessChain && n.op != OpInBoundsAccessChain) || n.len < 6 || !uniform_var.count(w[3])) continue;
        if (constant.count(w[5]) || set[w[3]] != 0 || !binding.count(w[3]) || binding[w[3]] >= 64) continue;
        auto t = type_of.find(w[5]);
        if (t == type_of.end() || !ints.count(t->second)) continue;
        clamp[n.at] = t->second;
        mask |= 1ull << binding[w[3]];
    }
    if (bindings) *bindings = mask;
    if (clamp.empty()) return false;
    std::vector<uint32_t> constants;
    for (auto& [type, id] : ints)
        if (!id) {
            id = bound++;
            constants.insert(constants.end(), {(4u << 16) | OpConstant, type, id, 4095});
        }
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == first_function) out.insert(out.end(), constants.begin(), constants.end());
        std::vector<uint32_t> w(in.begin() + n.at, in.begin() + n.at + n.len);
        if (auto c = clamp.find(n.at); c != clamp.end()) {
            uint32_t clamped = bound++;
            out.insert(out.end(), {(7u << 16) | OpExtInst, c->second, clamped, glsl, GlslUMin, w[5], ints[c->second]});
            w[5] = clamped;
        }
        out.insert(out.end(), w.begin(), w.end());
    }
    out[3] = bound;
    return true;
}

// ---- shared memory sized by the translator's default -------------------------------------------------------------
// The translator sizes a compute shader's shared memory with IGpuAccessor.QueryComputeSharedMemorySize(), which the
// harness does not answer: Ryujinx's default is 0xc000, so all 21 of Wonder's compute shaders that use shared memory
// declare uint[12288] (48 KB), past the M1's 32 KB of threadgroup memory, and Metal refuses the pipeline. The size
// the NVN compiler gave the program is in the control section (+0x754: 384..2312 bytes in Wonder; 0 exactly for the
// shaders without shared memory), the size Maxwell allocates per work group (docs/archive/nvn/DEBUG.md #23).
// resize_shared_memory() redeclares the Workgroup array with ceil(bytes / 4) elements. Only shrinks.
bool resize_shared_memory(const std::vector<uint32_t>& in, uint32_t bytes, std::vector<uint32_t>& out) {
    enum : uint32_t { OpTypeArray = 28, OpConstant = 43, StorageWorkgroup = 4 };
    if (in.size() < 5 || !bytes) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> constant, array, ptr;  // id -> (type, value) / (elem, length id)
    std::map<uint32_t, uint32_t> int_width;                                  //   / (storage, pointee)
    uint32_t var = 0, count = 0;
    size_t var_at = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypeInt && n.len == 4) int_width[w[1]] = w[2];
        else if (n.op == OpConstant && n.len == 4) constant[w[2]] = {w[1], w[3]};
        else if (n.op == OpTypeArray && n.len == 4) array[w[1]] = {w[2], w[3]};
        else if (n.op == OpTypePointer && n.len == 4) ptr[w[1]] = {w[2], w[3]};
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageWorkgroup) var = w[2], var_at = n.at, count++;
    }
    if (count != 1) return false;  // none, or several (the translator declares one): leave the module as it is
    auto p = ptr.find(in[var_at + 1]);
    if (p == ptr.end()) return false;
    auto a = array.find(p->second.second);
    if (a == array.end() || int_width[a->second.first] != 32) return false;  // uint[N]
    auto c = constant.find(a->second.second);
    uint32_t elems = (bytes + 3) / 4;
    if (c == constant.end() || elems >= c->second.second) return false;
    // Uses other than access chains (a load or copy of the whole array) would need the old type.
    // (OpLoad, OpStore, OpCopyMemory, OpFunctionCall, OpCopyObject, OpPtrAccessChain; the translator emits none.)
    for (const Inst& n : insts)
        if (other_pointer_use(&in[n.at], n.op, n.len, var, true)) return false;
    uint32_t bound = in[3], length = bound++, type = bound++, pointer = bound++;
    out.assign(in.begin(), in.begin() + var_at);
    out.insert(out.end(), {(4u << 16) | OpConstant, c->second.first, length, elems,         //
                           (4u << 16) | OpTypeArray, type, a->second.first, length,          //
                           (4u << 16) | OpTypePointer, pointer, StorageWorkgroup, type});
    out.insert(out.end(), in.begin() + var_at, in.end());
    out[var_at + 12 + 1] = pointer;  // the variable's result type
    out[3] = bound;
    return true;
}

// ---- fragment inputs the vertex stage does not write -----------------------------------------------------------
// Metal links the stages by name: a fragment input with no vertex output of the same location fails the pipeline
// ("Fragment input(s) `user(locn6)` mismatching vertex shader output type(s) or not written by vertex shader"). On
// Maxwell such an attribute reads as (0, 0, 0, 1): Ryujinx initializes every output the next stage reads to that
// (Translator.cs, InitializeOutputComponent). default_unwritten_inputs() turns the chosen input locations of a
// fragment module into Private variables with that initializer (docs/archive/nvn/DEBUG.md #24).
bool default_unwritten_inputs(const std::vector<uint32_t>& in, uint32_t locations, std::vector<uint32_t>& out) {
    enum : uint32_t { OpEntryPoint = 15, OpConstant = 43, OpConstantComposite = 44, StoragePrivate = 6, ModelFragment = 4 };
    if (in.size() < 5 || !locations) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, uint32_t> location, scalar_width;         // id -> Location / int or float width
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vec, ptr;  // id -> (component, count) / (storage, pointee)
    bool fragment = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPoint && n.len >= 3) fragment |= w[1] == ModelFragment;
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpTypeInt && n.len == 4) scalar_width[w[1]] = w[2];
        else if (n.op == OpTypeFloat && n.len == 3) scalar_width[w[1]] = w[2];
        else if (n.op == OpTypeVector && n.len == 4) vec[w[1]] = {w[2], w[3]};
        else if (n.op == OpTypePointer && n.len == 4) ptr[w[1]] = {w[2], w[3]};
    }
    if (!fragment) return false;

    // The variables: 32-bit scalars or vectors (the translator declares user attributes as vec4).
    uint32_t bound = in[3];
    std::map<uint32_t, uint32_t> patched;  // variable or access chain id -> Private pointer type of its result
    std::map<uint32_t, uint32_t> private_ptr;  // pointee -> new Private pointer type
    std::map<uint32_t, uint32_t> init;         // variable -> initializer
    std::vector<uint32_t> globals;
    size_t insert_at = in.size();
    auto pointer_to = [&](uint32_t pointee) {
        auto& id = private_ptr[pointee];
        if (!id) {
            id = bound++;
            globals.insert(globals.end(), {(4u << 16) | OpTypePointer, id, StoragePrivate, pointee});
        }
        return id;
    };
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len != 4 || w[3] != StorageInput) continue;
        auto l = location.find(w[2]);
        if (l == location.end() || l->second >= 32 || !(locations >> l->second & 1)) continue;
        auto p = ptr.find(w[1]);
        if (p == ptr.end()) return false;
        uint32_t pointee = p->second.second, comp = pointee, count = 1;
        if (auto v = vec.find(pointee); v != vec.end()) comp = v->second.first, count = v->second.second;
        if (scalar_width[comp] != 32) return false;  // arrays, blocks, 16/64-bit: not produced for attributes
        insert_at = std::min(insert_at, n.at);
        uint32_t zero = bound++, one = bound++, value = bound++;
        globals.insert(globals.end(), {(4u << 16) | OpConstant, comp, zero, 0u,  //
                                       (4u << 16) | OpConstant, comp, one, 0x3f800000u});  // 1.0f (bits for ints)
        if (count == 1) {
            value = zero;
        } else {
            globals.insert(globals.end(), {((3u + count) << 16) | OpConstantComposite, pointee, value});
            for (uint32_t k = 0; k < count; k++) globals.push_back(k == 3 ? one : zero);
        }
        pointer_to(comp);  // for the access chains
        patched[w[2]] = pointer_to(pointee);
        init[w[2]] = value;
    }
    if (patched.empty()) return false;

    const bool interface_private = in[1] >= 0x10400;  // SPIR-V 1.4+: every global is in the interface list
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == insert_at) out.insert(out.end(), globals.begin(), globals.end());
        std::vector<uint32_t> w(in.begin() + n.at, in.begin() + n.at + n.len);
        if (n.op == OpEntryPoint && !interface_private) {
            size_t k = 3;
            while (k < w.size() && (w[k] >> 24) != 0) k++;  // the name: a nul-terminated string
            std::vector<uint32_t> e(w.begin(), w.begin() + std::min(k + 1, w.size()));
            for (size_t j = k + 1; j < w.size(); j++)
                if (!patched.count(w[j])) e.push_back(w[j]);
            e[0] = uint32_t(e.size()) << 16 | OpEntryPoint;
            w = std::move(e);
        } else if (n.op == OpDecorate && n.len >= 3 && patched.count(w[1])) {
            continue;  // Location, interpolation: Input/Output only
        } else if (n.op == OpVariable && patched.count(w[2]) && init.count(w[2])) {
            w = {(5u << 16) | OpVariable, patched[w[2]], w[2], StoragePrivate, init[w[2]]};
        } else if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 4 && patched.count(w[3])) {
            auto p = ptr.find(w[1]);
            if (p == ptr.end() || !private_ptr.count(p->second.second)) return false;
            w[1] = patched[w[2]] = private_ptr[p->second.second];
        } else {
            for (auto& [id, type] : patched)
                if (other_pointer_use(w.data(), n.op, n.len, id, false)) return false;
        }
        out.insert(out.end(), w.begin(), w.end());
    }
    out[3] = bound;
    return true;
}

// ---- vertex outputs start at zero ----------------------------------------------------------------------------------
// A vertex shader may write an output only on some paths, or only some of its components: the rest is undefined in
// SPIR-V. On Maxwell it reads back as whatever the register held (zero in practice); Mesa ANV leaves the garbage of
// the previous vertex's registers there, NaN included, and the fragment shader that normalizes the tangent frame turns
// it into NaN lighting (the black characters and terrain on the Intel UHD, docs/archive/nvn/studies/intel-60fps-1710.md).
// zero_vertex_outputs() gives every Location output variable of a vertex module a zero initializer. Defined results
// are unchanged: only a component that was never stored reads 0 instead of an undefined value.
bool zero_vertex_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    enum : uint32_t { OpEntryPointOp = 15, OpConstantNull = 46, StorageOutput = 3, ModelVertex = 0 };
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, uint32_t> location, pointee;  // variable -> Location; pointer type -> pointee type
    bool vertex = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPointOp && n.len >= 3) vertex |= w[1] == ModelVertex;
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageOutput) pointee[w[1]] = w[3];
    }
    if (!vertex) return false;
    uint32_t bound = in[3];
    bool any = false;
    out.assign(in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpVariable && n.len == 4 && w[3] == StorageOutput && location.count(w[2]) && pointee.count(w[1])) {
            uint32_t zero = bound++;
            out.insert(out.end(), {(3u << 16) | OpConstantNull, pointee[w[1]], zero});
            out.insert(out.end(), {(5u << 16) | OpVariable, w[1], w[2], StorageOutput, zero});
            any = true;
            continue;
        }
        out.insert(out.end(), w, w + n.len);
    }
    out[3] = bound;
    return any;
}

// ---- position invariance ----------------------------------------------------------------------------------------
// The colour pass after a depth pre-pass tests EQUAL against depth another vertex shader wrote (DEBUG-2 #60). Two
// shaders computing the same position can round differently when the driver contracts or reassociates them
// differently; Vulkan guarantees equal results only for outputs decorated Invariant. NVIDIA's compiler happens to
// agree, Mesa ANV (Intel) does not: the characters and the terrain stayed the pre-pass's black (0,0,0,1) on an Intel
// UHD. Decorating every vertex shader's Position Invariant makes the two passes agree (on by default, NX_INVARIANT=0 off).
bool make_position_invariant(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    constexpr uint32_t OpMemberDecorate = 72, DecorationBuiltIn = 11, DecorationInvariant = 18, BuiltInPosition = 0;
    if (in.size() < 5) return false;
    out.assign(in.begin(), in.begin() + 5);
    bool changed = false;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || i + len > in.size()) return false;
        out.insert(out.end(), in.begin() + i, in.begin() + i + len);
        const uint32_t* w = &in[i];
        if (op == OpDecorate && len == 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition) {
            out.insert(out.end(), {(3u << 16) | OpDecorate, w[1], DecorationInvariant});
            changed = true;
        } else if (op == OpMemberDecorate && len == 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition) {
            out.insert(out.end(), {(4u << 16) | OpMemberDecorate, w[1], w[2], DecorationInvariant});
            changed = true;
        }
        i += len;
    }
    return changed;
}

// ---- float controls ---------------------------------------------------------------------------------------------
// Maxwell shaders keep infinities and NaNs (a min against 1/0, a compare of a NaN), and the translated code relies on it.
// Vulkan lets a compiler assume finite values unless the shader asks for SignedZeroInfNanPreserve (SPV_KHR_float_controls).
// NVIDIA's compiler preserves them anyway; Mesa ANV may fold them away (NX_FLOAT_CONTROLS=1 turns this on, 0 off).
bool preserve_inf_nan(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    constexpr uint32_t OpCapabilityOp = 17, OpExtensionOp = 10, OpEntryPointOp = 15, OpExecutionModeOp = 16;
    constexpr uint32_t CapSignedZeroInfNanPreserve = 4466, ModeSignedZeroInfNanPreserve = 4461;
    if (in.size() < 5) return false;
    size_t after_cap = 0, after_ext = 0, after_mode = 0;
    uint32_t entry = 0;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || i + len > in.size()) return false;
        if (op == OpCapabilityOp) after_cap = i + len;
        else if (op == OpExtensionOp) after_ext = i + len;
        else if (op == OpEntryPointOp) { if (!entry) entry = in[i + 2]; after_mode = std::max(after_mode, i + len); }
        else if (op == OpExecutionModeOp) after_mode = std::max(after_mode, i + len);
        i += len;
    }
    if (!entry || !after_cap || !after_mode) return false;
    if (!after_ext) after_ext = after_cap;  // extensions follow the capabilities
    // "SPV_KHR_float_controls" as a literal string: 22 characters and a nul, 6 words.
    const char name[] = "SPV_KHR_float_controls";
    std::vector<uint32_t> ext((sizeof name + 3) / 4, 0);
    std::memcpy(ext.data(), name, sizeof name);
    ext.insert(ext.begin(), (uint32_t(ext.size() + 1) << 16) | OpExtensionOp);
    const std::vector<uint32_t> cap = {(2u << 16) | OpCapabilityOp, CapSignedZeroInfNanPreserve};
    const std::vector<uint32_t> mode = {(4u << 16) | OpExecutionModeOp, entry, ModeSignedZeroInfNanPreserve, 32};
    // The three insertion points in file order: capabilities, extensions, execution modes.
    size_t p[3] = {after_cap, std::max(after_ext, after_cap), after_mode};
    out.assign(in.begin(), in.begin() + long(p[0]));
    out.insert(out.end(), cap.begin(), cap.end());
    out.insert(out.end(), in.begin() + long(p[0]), in.begin() + long(p[1]));
    out.insert(out.end(), ext.begin(), ext.end());
    out.insert(out.end(), in.begin() + long(p[1]), in.begin() + long(p[2]));
    out.insert(out.end(), mode.begin(), mode.end());
    out.insert(out.end(), in.begin() + long(p[2]), in.end());
    return true;
}

uint32_t unwritten_inputs(const ShaderInfo* vs, const ShaderInfo* fs) {
    uint32_t mask = 0;
    if (!vs || !fs) return 0;
    for (const ShaderIo& in : fs->refl.inputs)
        for (uint32_t l = in.location; l < in.location + in.array && l < 32; l++) mask |= 1u << l;
    for (const ShaderIo& out : vs->refl.outputs)
        for (uint32_t l = out.location; l < out.location + out.array && l < 32; l++) mask &= ~(1u << l);
    return mask;
}

std::vector<uint32_t> patched_spirv(const ShaderInfo* s, const uint8_t* kinds, uint32_t unwritten, bool* int_inputs) {
    std::vector<uint32_t> code = s->spirv, next;  // each pass reads `code` and leaves its result in `next`
    bool is_int = kinds && patch_int_inputs(code, kinds, next);
    if (is_int) code.swap(next);
    if (int_inputs) *int_inputs = is_int;
    if (s->stage == ShaderStage::Compute && s->control.shared_size &&
        resize_shared_memory(code, s->control.shared_size, next)) {
        code.swap(next);
        log("nvn/vk: shader %s: shared memory declared as %u bytes", s->hash.c_str(), s->control.shared_size);
    }
    if (unwritten) {
        bool ok = default_unwritten_inputs(code, unwritten, next);
        if (ok) code.swap(next);
        log("nvn/vk: shader %s: inputs %#x not written by the vertex stage%s", s->hash.c_str(), unwritten,
            ok ? " read (0, 0, 0, 1)" : ", left as they are");
    }
    if (clamp_uniform_indices(code, next, nullptr)) code.swap(next);
    static const bool zero_outputs = [] {  // NX_ZERO_VS_OUTPUTS=0 turns it off (1 turns it on for MoltenVK, not tried)
        const char* e = std::getenv("NX_ZERO_VS_OUTPUTS");
#if defined(__APPLE__)
        return e && e[0] == '1';
#else
        return !(e && e[0] == '0');
#endif
    }();
    if (zero_outputs && s->stage == ShaderStage::Vertex && zero_vertex_outputs(code, next)) code.swap(next);
    static const bool invariant = [] {
        // On by default (NX_INVARIANT=0 turns it off). It did not change the Intel silhouettes, but Witch Time and
        // B3-Express both fix the late-game Kraken goo (a black slab where a second pass misses the first pass's
        // depth) with invariant positions, and it costs nothing measurable.
        const char* e = std::getenv("NX_INVARIANT");
        return !(e && e[0] == '0');
    }();
    if (invariant && s->stage == ShaderStage::Vertex && make_position_invariant(code, next)) code.swap(next);
    static const bool fc = [] {
        const char* e = std::getenv("NX_FLOAT_CONTROLS");
        return e && std::atoi(e) != 0;
    }();
    if (fc && s->stage != ShaderStage::Compute && preserve_inf_nan(code, next)) code.swap(next);
    return code;
}

uint64_t dynamic_uniform_bindings(const ShaderInfo* s) {
    if (s->masks_known) return s->dynamic_uniforms;
    static std::mutex lock;
    static std::map<const ShaderInfo*, uint64_t> cache;
    std::lock_guard lk(lock);
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    std::vector<uint32_t> unused;
    uint64_t mask = 0;
    clamp_uniform_indices(s->spirv, unused, &mask);
    return cache[s] = mask;
}

// Bindings (set 0..1, binding < 64) of storage buffers the shader writes: OpStore or an atomic through a pointer into
// them. Compute results are copied back to guest memory after the submit only for these, and a submit with
// write-backs is waited for before the next replay (NX_ASYNC_GPU): read-only storage counted too (DEBUG.md #32).
uint64_t written_storage_bindings(const ShaderInfo* s) {
    if (s->masks_known) return s->written_storage;
    static std::mutex lock;
    static std::map<const ShaderInfo*, uint64_t> cache;
    std::lock_guard lk(lock);
    if (auto it = cache.find(s); it != cache.end()) return it->second;
    const std::vector<uint32_t>& in = s->spirv;
    std::map<uint32_t, uint32_t> binding, base;  // variable -> binding; pointer -> the variable it points into
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || i + len > in.size()) break;
        const uint32_t* w = &in[i];
        if (op == 71 && len >= 4 && w[2] == 33) binding[w[1]] = w[3];  // OpDecorate Binding
        if (op == 59 && len >= 4) base[w[2]] = w[2];                    // OpVariable
        if ((op == 65 || op == 66 || op == 67) && len >= 4 && base.count(w[3])) base[w[2]] = base[w[3]];
        i += len;
    }
    uint64_t mask = 0;
    auto mark = [&](uint32_t ptr) {
        auto b = base.find(ptr);
        if (b == base.end()) mask = ~0ull;  // a pointer not traced to a variable: assume every binding is written
        else if (binding.count(b->second) && binding[b->second] < 64) mask |= 1ull << binding[b->second];
    };
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || i + len > in.size()) break;
        const uint32_t* w = &in[i];
        if ((op == 62 || op == 63 || op == 228) && len >= 3) mark(w[1]);  // OpStore, OpCopyMemory, OpAtomicStore
        else if (((op >= 229 && op <= 242) || op == 5614 || op == 5615 || op == 6035) && len >= 4)
            mark(w[3]);  // read-modify-write atomics (float ones too)
        i += len;
    }
    return cache[s] = mask;
}

}  // namespace nx::nvn

namespace nx::nvn {

// ---- NVN 0x3f (R11G11B10F): the console's store conversion --------------------------------------------------------
// R11G11B10F on the console is an *unsigned* float format: a fragment result that is NaN or negative is converted to
// 0 when it is stored, and +Inf or anything above the format's maximum to that maximum, so a NaN can never reach the
// next pass. This runtime stores 0x3f as RGBA16F (nvn_format.cpp: MoltenVK never clamps and no MTLPixelFormat
// reproduces the conversion — d2r-mobile's Metal NVN layer keeps NaN in MTLPixelFormatRG11B10Float and even uses NaN
// as a clear sentinel), so the conversion is emulated here: wrap the fragment entry in a function that converts each
// colour output before the module returns.
//
// `locations` is the set of colour attachments that are 0x3f (PipelineKey::ufloat_colors), so a pass writing a signed
// HDR attachment as well leaves that one alone. The pipeline's colour write mask still applies the game's channel
// mask, so components the shader leaves unwritten stay as masked as before. Alpha is converted too: a 0x3f attachment
// has no alpha, and the destination-alpha blend emulation reads it as 1 regardless.
bool patch_ufloat_outputs(const std::vector<uint32_t>& in, uint32_t locations, std::vector<uint32_t>& out) {
    constexpr uint32_t OpEntryPoint = 15, OpExecutionMode = 16, OpTypeVoid = 19, OpTypeBool = 20, OpTypeFloat = 22, OpTypeVector = 23,
                       OpTypePointer = 32, OpTypeFunction = 33, OpConstant = 43, OpConstantComposite = 44,
                       OpConstantNull = 46, OpFunction = 54, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61,
                       OpStore = 62, OpDecorate = 71, OpIsNan = 156, OpSelect = 169, OpFOrdLessThan = 184,
                       OpFOrdGreaterThan = 186, OpLabel = 248, OpReturn = 253;  // FMax/FMin are GLSL.std.450, not core
    constexpr uint32_t StorageClassOutput = 3, DecorationLocation = 30;
    const float kMaxRG = 65024.0f;   // 11-bit: 6-bit mantissa, 5-bit exponent
    const float kMaxB = 63488.0f;    // 10-bit: 5-bit mantissa
    const float kMaxPass = 3.402823466e+38f;  // alpha has no 0x3f storage: finite values pass through
    if (in.size() < 5 || in[0] != 0x07230203u || !(locations & 0xffu)) return false;

    std::map<uint32_t, uint32_t> type_op;                  // type id -> opcode
    std::map<uint32_t, std::vector<uint32_t>> type_words;  // type id -> its instruction
    std::map<uint32_t, uint32_t> loc;                      // id -> Location decoration
    std::map<uint32_t, uint32_t> floats, ints;             // bit pattern / value -> constant id
    std::vector<std::pair<uint32_t, uint32_t>> out_vars;  // (variable id, its declared pointer type)
    uint32_t entry = 0, void_type = 0, float_type = 0, bool_type = 0, int_type = 0;
    size_t entry_at = in.size(), first_function = in.size();
    for (size_t i = 5; i < in.size();) {
        const uint32_t n = in[i] >> 16, op = in[i] & 0xffff;
        if (!n || n > in.size() - i) return false;
        const uint32_t* w = &in[i];
        if (op >= 19 && op <= 33 && n >= 2) type_op[w[1]] = op, type_words[w[1]].assign(w, w + n);
        if (op == OpEntryPoint && n >= 4 && w[1] == 4 && !entry) entry = w[2], entry_at = i;
        else if (op == OpTypeVoid && n == 2) void_type = w[1];
        else if (op == OpTypeBool && n == 2) bool_type = w[1];
        else if (op == OpTypeFloat && n == 3 && w[2] == 32) float_type = w[1];
        else if (op == OpConstant && n == 4) {
            if (type_op.count(w[1]) && type_op[w[1]] == OpTypeFloat) floats[w[3]] = w[1];
            if (type_op.count(w[1]) && type_op[w[1]] == 21) ints[w[3]] = w[1];
        } else if (op == OpDecorate && n == 4 && w[2] == DecorationLocation) loc[w[1]] = w[3];
        else if (op == OpVariable && n == 4 && w[3] == StorageClassOutput) out_vars.push_back({w[2], w[1]});
        if (op == OpFunction && first_function == in.size()) first_function = i;
        i += n;
    }
    if (!entry || !void_type || !float_type || first_function == in.size()) return false;
    struct Out { uint32_t var, type, comps; };
    std::vector<Out> convert;
    for (const auto& [var, ptr_type] : out_vars) {
        auto l = loc.find(var);
        if (l == loc.end() || l->second >= 32 || !(locations >> l->second & 1u)) continue;
        auto p = type_words.find(ptr_type);
        if (p == type_words.end() || type_op[ptr_type] != OpTypePointer || p->second.size() < 4) continue;
        const uint32_t type = p->second[3];
        auto t = type_words.find(type);
        if (t == type_words.end()) continue;
        uint32_t comps = 0;
        if (type_op[type] == OpTypeFloat) comps = 1;
        else if (type_op[type] == OpTypeVector && t->second.size() >= 4 && type_op.count(t->second[2]) &&
                 type_op[t->second[2]] == OpTypeFloat)
            comps = t->second[3];
        else continue;  // integer output: never a 0x3f attachment
        if (comps < 1 || comps > 4) continue;
        convert.push_back({var, type, comps});
    }
    if (convert.empty()) { out = in; return true; }

    uint32_t bound = in[3];
    if (!bound || bound > 0xffffff00u) return false;
    std::vector<uint32_t> globals;
    if (!bool_type) {
        bool_type = bound++;
        globals.insert(globals.end(), {(2u << 16) | OpTypeBool, bool_type});
    }
    (void)int_type;
    (void)ints;
    auto fconst = [&](float f) -> uint32_t {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        auto it = floats.find(bits);
        if (it != floats.end()) return it->second;
        const uint32_t id = bound++;
        globals.insert(globals.end(), {(4u << 16) | OpConstant, float_type, id, bits});
        return floats[bits] = id;
    };
    std::map<uint32_t, uint32_t> null_of, max_of;  // output type id -> constant
    for (const Out& o : convert) {
        if (null_of.count(o.type)) continue;
        const uint32_t zero = bound++;
        globals.insert(globals.end(), {(3u << 16) | OpConstantNull, o.type, zero});
        null_of[o.type] = zero;
        if (o.comps == 1) {
            max_of[o.type] = fconst(kMaxPass);
            continue;
        }
        const uint32_t id = bound++;
        std::vector<uint32_t> ins{(uint32_t(3 + o.comps) << 16) | OpConstantComposite, o.type, id};
        for (uint32_t k = 0; k < o.comps; k++) ins.push_back(fconst(k == 2 ? kMaxB : k == 3 ? kMaxPass : kMaxRG));
        globals.insert(globals.end(), ins.begin(), ins.end());
        max_of[o.type] = id;
    }
    // A bool vector for OpIsNan on a vector output: reuse the module's own when it has one.
    std::map<uint32_t, uint32_t> bool_vec;
    auto bool_vector = [&](uint32_t n) -> uint32_t {
        if (n == 1) return bool_type;
        if (bool_vec.count(n)) return bool_vec[n];
        for (const auto& [id, op] : type_op)
            if (op == OpTypeVector && type_words[id].size() >= 4 && type_words[id][2] == bool_type &&
                type_words[id][3] == n)
                return bool_vec[n] = id;
        const uint32_t id = bound++;
        globals.insert(globals.end(), {(4u << 16) | OpTypeVector, id, bool_type, n});
        return bool_vec[n] = id;
    };
    uint32_t fn_type = 0;  // void(void): a "duplicate non-aggregate type" is invalid, so reuse the module's own
    for (const auto& [id, op] : type_op)
        if (op == OpTypeFunction && type_words[id].size() == 3 && type_words[id][2] == void_type) fn_type = id;
    const uint32_t wrapper = bound++, label = bound++, call_result = bound++;
    if (!fn_type) {
        fn_type = bound++;
        globals.insert(globals.end(), {(3u << 16) | OpTypeFunction, fn_type, void_type});
    }
    std::vector<uint32_t> fn;
    fn.insert(fn.end(), {(5u << 16) | OpFunction, void_type, wrapper, 0u, fn_type});
    fn.insert(fn.end(), {(2u << 16) | OpLabel, label});
    fn.insert(fn.end(), {(4u << 16) | OpFunctionCall, void_type, call_result, entry});  // void result: id unused
    for (const Out& o : convert) {  // clamped = min(max(isnan(v) ? 0 : v, 0), format max)
        // v -> (isnan ? 0 : v) -> (v < 0 ? 0 : v) -> (v > format max ? format max : v). The NaN test comes first, so
        // the ordered compares only ever see ordinary numbers.
        const uint32_t v = bound++, isnan = bound++, sel = bound++, less = bound++, clamped = bound++, over = bound++,
                       final = bound++;
        const uint32_t bvec = bool_vector(o.comps);
        fn.insert(fn.end(), {(4u << 16) | OpLoad, o.type, v, o.var});
        fn.insert(fn.end(), {(4u << 16) | OpIsNan, bvec, isnan, v});
        fn.insert(fn.end(), {(6u << 16) | OpSelect, o.type, sel, isnan, null_of[o.type], v});
        fn.insert(fn.end(), {(5u << 16) | OpFOrdLessThan, bvec, less, sel, null_of[o.type]});
        fn.insert(fn.end(), {(6u << 16) | OpSelect, o.type, clamped, less, null_of[o.type], sel});
        fn.insert(fn.end(), {(5u << 16) | OpFOrdGreaterThan, bvec, over, clamped, max_of[o.type]});
        fn.insert(fn.end(), {(6u << 16) | OpSelect, o.type, final, over, max_of[o.type], clamped});
        fn.insert(fn.end(), {(3u << 16) | OpStore, o.var, final});
    }
    fn.insert(fn.end(), {(1u << 16) | OpReturn});
    fn.insert(fn.end(), {(1u << 16) | 56u});  // OpFunctionEnd

    out.clear();
    out.reserve(in.size() + globals.size() + fn.size() + 4);
    out.insert(out.end(), in.begin(), in.begin() + long(first_function));
    out.insert(out.end(), globals.begin(), globals.end());
    out.insert(out.end(), in.begin() + long(first_function), in.end());
    out[3] = bound;
    bool renamed = false;
    for (size_t i = 5; i < out.size();) {  // the entry point (and its execution modes) now name the wrapper
        const uint32_t n = out[i] >> 16, op = out[i] & 0xffff;
        if (!n || n > out.size() - i) return false;
        if (op == OpEntryPoint && n >= 4 && i >= (size_t)entry_at && out[i + 2] == entry) out[i + 2] = wrapper, renamed = true;
        else if (op == OpExecutionMode && n >= 3 && out[i + 1] == entry) out[i + 1] = wrapper;
        i += n;
    }
    if (!renamed) return false;
    out.insert(out.end(), fn.begin(), fn.end());
    return true;
}

}  // namespace nx::nvn
