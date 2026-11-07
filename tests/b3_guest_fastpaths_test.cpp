#include <nx/b3_guest_fastpaths.h>
#include <nx/cpu.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void main_00B382C0(nx::CPU&);
void main_007E1FE4(nx::CPU&);
uint64_t test_guest_base = 0;
int test_draw_calls = 0;

namespace {

void check(bool ok, const char* text) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", text); std::abort(); }
}

uint64_t address(const void* p) { return uint64_t(reinterpret_cast<uintptr_t>(p)); }
uint32_t random32() {
    static uint64_t state = 0x61c8864680b583ebull;
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return uint32_t(state);
}

struct Fixture {
    alignas(16) std::array<uint8_t, 32768> stack{};
    alignas(16) std::array<uint8_t, 1024> geometry{};
    uint64_t sp() const { return address(stack.data() + stack.size() - 64); }
    nx::CPU cpu() const {
        nx::CPU c{};
        for (int i = 0; i < 32; ++i) {
            c.x[i] = 0xfedc00000000ull + uint64_t(i) * 0x1234;
            c.v[i].u64[0] = 0x13579bdf00000000ull + uint64_t(i);
            c.v[i].u64[1] = 0x2468ace000000000ull + uint64_t(i);
        }
        c.x[31] = sp();
        c.x[3] = c.x[4] = c.x[6] = address(geometry.data());
        c.nf = 1; c.zf = 0; c.cf = 0; c.vf = 1;
        c.fpcr = 7; c.fpsr = 11; c.tpidr = 13; c.tpidrro = 17;
        c.excl_addr = 23; c.excl_val = 29; c.excl_val_hi = 31;
        return c;
    }
};

void same_cpu(const nx::CPU& a, const nx::CPU& b, const char* message) {
    for (int i = 0; i < 32; ++i) {
        check(a.x[i] == b.x[i], message);
        check(a.v[i].u64[0] == b.v[i].u64[0] && a.v[i].u64[1] == b.v[i].u64[1], message);
    }
    check(a.nf == b.nf && a.zf == b.zf && a.cf == b.cf && a.vf == b.vf, message);
    check(a.fpcr == b.fpcr && a.fpsr == b.fpsr && a.tpidr == b.tpidr && a.tpidrro == b.tpidrro &&
          a.excl_addr == b.excl_addr && a.excl_val == b.excl_val && a.excl_val_hi == b.excl_val_hi, message);
}

void same_saved_and_flags(const nx::CPU& got, const nx::CPU& original) {
    for (int i = 19; i < 32; ++i) check(got.x[i] == original.x[i], "callee-saved GPR/SP");
    for (int i = 8; i < 16; ++i) {
        check(got.v[i].u64[0] == original.v[i].u64[0], "callee-saved FP low");
        check(got.v[i].u64[1] == original.v[i].u64[1], "FP upper half");
    }
    check(got.nf == original.nf && got.zf == original.zf && got.cf == original.cf && got.vf == original.vf,
          "NZCV differs from generated body");
    check(got.fpcr == original.fpcr && got.fpsr == original.fpsr, "FP status differs");
}

uint32_t compact_bits(uint32_t n, unsigned start) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= ((n >> (2 * i + start)) & 1u) << i;
    return out;
}

void test_morton() {
    Fixture f;
    for (int i = 0; i < 4096; ++i) {
        const uint32_t value = i < 32 ? 1u << i : i < 64 ? ~(1u << (i - 32)) : random32();
        std::array<uint8_t, 24> fast{}, original{};
        fast.fill(0xa5); original.fill(0xa5);
        nx::CPU a = f.cpu(), b = a;
        a.x[0] = b.x[0] = (uint64_t(random32()) << 32) | value;
        a.x[1] = address(fast.data() + 1); a.x[2] = address(fast.data() + 11);
        b.x[1] = address(original.data() + 1); b.x[2] = address(original.data() + 11);
        check(nx::b3::try_morton_split(a), "Morton fast hit");
        main_00B382C0(b);
        check(fast == original, "Morton output bytes versus emitted body");
        check(nx::ld<uint32_t>(a.x[1]) == compact_bits(value, 0), "Morton even oracle");
        check(nx::ld<uint32_t>(a.x[2]) == compact_bits(value, 1), "Morton odd oracle");
        check(a.x[0] == b.x[0] && a.x[8] == b.x[8] && a.x[9] == b.x[9], "Morton return/scratch");
        same_saved_and_flags(a, b);
    }

    for (int delta = -3; delta <= 3; ++delta) {
        std::array<uint8_t, 32> fast{}, original{};
        fast.fill(0xa5); original = fast;
        nx::CPU a = f.cpu(), b = a;
        a.x[0] = b.x[0] = 0xa43e82d1;
        a.x[1] = address(fast.data() + 12); a.x[2] = address(fast.data() + 12 + delta);
        b.x[1] = address(original.data() + 12); b.x[2] = address(original.data() + 12 + delta);
        const nx::CPU before = a;
        check(!nx::b3::try_morton_split(a), "overlap must fall back");
        same_cpu(a, before, "overlap fallback mutates CPU");
        check(fast == original, "overlap fallback mutates memory");
        main_00B382C0(a); main_00B382C0(b);
        check(fast == original && a.x[0] == b.x[0], "overlap original fallback");
        same_saved_and_flags(a, b);
    }
    for (int choice = 0; choice < 4; ++choice) {
        nx::CPU c = f.cpu();
        std::array<uint8_t, 16> out{};
        c.x[1] = address(out.data()); c.x[2] = address(out.data() + 8);
        if (choice == 0) c.x[1] = 0;
        if (choice == 1) c.x[1] = UINT64_MAX - 1;
        if (choice == 2) c.x[1] = c.x[31] - 4;
        if (choice == 3) c.x[2] = c.x[31] - 32;
        const auto old = c;
        const auto memory = out;
        check(!nx::b3::try_morton_split(c), "Morton invalid/stack output fallback");
        same_cpu(c, old, "Morton fallback CPU");
        check(out == memory, "Morton fallback memory");
    }
}

// Deliberately models the recursive 32-bit child indices and leaf comparisons,
// rather than scanning the optimized bounding rectangle.
int quad_oracle(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                uint32_t width, uint32_t height, uint32_t index, int depth) {
    if (depth == 0) {
        const uint32_t x = compact_bits(index, 0), y = compact_bits(index, 1);
        if (x > width || y > height) return 0;
        const uint32_t offset = x + y * width;
        return a[offset] != b[offset];
    }
    int result = 1;
    for (uint32_t i = 0; i < 4; ++i)
        result &= quad_oracle(a, b, width, height, (index << 2) | i, depth - 1);
    return result;
}

void install_quad(Fixture& f, nx::CPU& c, const std::vector<uint8_t>& a,
                  const std::vector<uint8_t>& b, uint32_t width, uint32_t height,
                  uint32_t index, uint32_t depth) {
    c.x[1] = width; c.x[2] = height; c.x[7] = depth;
    nx::st<uint32_t>(f.sp(), index);
    nx::st<uint64_t>(f.sp() + 8, address(a.data()));
    nx::st<uint64_t>(f.sp() + 16, address(b.data()));
}

void setup_guest_globals(std::vector<uint8_t>& memory, std::array<uint8_t, 8192>& dummy) {
    test_guest_base = address(memory.data());
    // Synthetic relocated pointers needed by the emitted zero-result path.
    // This is not a simulation of the renderer.
    for (uint64_t p = 0x249c000; p < 0x249f000; p += 8)
        nx::st<uint64_t>(test_guest_base + p, address(dummy.data()));
}

void quad_case(uint32_t width, uint32_t height, uint32_t index, uint32_t depth,
               std::vector<uint8_t> a, std::vector<uint8_t> b, bool expected_hit,
               bool compare_original = true) {
    Fixture f;
    nx::CPU fast = f.cpu();
    install_quad(f, fast, a, b, width, height, index, depth);
    const nx::CPU before = fast;
    const auto old_a = a, old_b = b;
    const auto old_stack = f.stack;
    const bool hit = nx::b3::try_quad_walk(fast);
    check(hit == expected_hit, "quadtree hit/fallback classification");
    check(a == old_a && b == old_b, "quadtree modified maps");
    check(f.stack == old_stack, "quadtree modified guest stack");
    if (!hit) { same_cpu(fast, before, "quadtree fallback CPU"); return; }
    if (depth <= 5)
        check(fast.x[0] == uint64_t(quad_oracle(a, b, width, height, index, int(depth))),
              "quadtree recursive oracle");
    if (compare_original) {
        nx::CPU original = before;
        test_draw_calls = 0;
        main_007E1FE4(original);
        check(test_draw_calls == 0, "homogeneous quadtree unexpectedly drew");
        check(fast.x[0] == original.x[0], "quadtree result vs emitted body");
        same_saved_and_flags(fast, original);
    }
}

void test_quad() {
    std::vector<uint8_t> a(1000), b(1000);
    for (size_t i = 0; i < a.size(); ++i) a[i] = b[i] = uint8_t(i * 37 + 91);
    quad_case(8, 8, 0, 2, a, b, true);       // all equal
    quad_case(2, 2, 0, 2, a, b, true);       // clipped, all equal
    quad_case(2, 2, 3, 2, a, b, true);       // entirely outside
    quad_case(0, 0, 0, 2, a, b, true);       // width is also stride
    quad_case(0, 0, 0, 15, a, b, true, false); // highest supported depth
    for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(a[i] ^ 0xff);
    quad_case(8, 8, 0, 2, a, b, true);       // all different
    quad_case(2, 2, 0, 2, a, b, false);      // clipped with differing bytes
    b = a; b[3] ^= 1;
    quad_case(8, 8, 0, 2, a, b, false);      // mixed
    quad_case(8, 8, 0x40000000u, 2, a, b, false); // recursive index truncation
    quad_case(0xffffffffu, 100, 2, 2, a, b, false); // 32-bit leaf offset overflow
    quad_case(8, 8, 0, 1, a, b, false);
    quad_case(8, 8, 0, 16, a, b, false);

    // Exercise mixed-node fall-through through the extracted production body.
    // The draw-related callees are instrumented stubs, not the real renderer.
    {
        Fixture f;
        nx::CPU original = f.cpu();
        install_quad(f, original, a, b, 8, 8, 0, 2);
        nx::CPU guarded = original;
        const auto initial_stack = f.stack;
        test_draw_calls = 0;
        main_007E1FE4(original);
        const int original_calls = test_draw_calls;
        const auto final_stack = f.stack;
        f.stack = initial_stack;
        test_draw_calls = 0;
        if (!nx::b3::try_quad_walk(guarded)) main_007E1FE4(guarded);
        check(original_calls > 0 && test_draw_calls == original_calls, "mixed node draw fallback");
        same_cpu(guarded, original, "mixed node original fallback CPU");
        check(f.stack == final_stack, "mixed node original fallback stack");
        check(original.x[0] == uint64_t(quad_oracle(a, b, 8, 8, 0, 2)), "mixed node oracle");
    }

    // Vary dimensions, Morton origins and payload bytes; compare each accepted
    // result against both recursion and the actual emitted function.
    for (int i = 0; i < 120; ++i) {
        const uint32_t w = random32() % 20, h = random32() % 20;
        const uint32_t idx = random32() % 16, depth = 2 + random32() % 2;
        const size_t size = size_t(w) * h + w + 1;
        std::vector<uint8_t> aa(size), bb(size);
        for (size_t j = 0; j < size; ++j) {
            aa[j] = uint8_t(random32());
            bb[j] = (i & 1) ? uint8_t(aa[j] ^ 0xff) : aa[j];
        }
        const uint32_t x = compact_bits(idx, 0) << depth;
        const uint32_t y = compact_bits(idx, 1) << depth;
        const uint32_t n = 1u << depth;
        const bool outside = x > w || y > h;
        const bool clipped = uint64_t(x) + n - 1 > w || uint64_t(y) + n - 1 > h;
        const bool hit = !(i & 1) || outside || !clipped;
        quad_case(w, h, idx, depth, aa, bb, hit);
    }

    Fixture f;
    nx::CPU c = f.cpu();
    install_quad(f, c, a, a, 8, 8, 0, 2);
    nx::st<uint64_t>(f.sp() + 8, f.sp() - 128);
    const auto before = c;
    const auto stack_before = f.stack;
    check(!nx::b3::try_quad_walk(c), "quadtree map/stack overlap fallback");
    same_cpu(c, before, "stack overlap CPU mutation");
    check(f.stack == stack_before, "stack overlap memory mutation");
}
}  // namespace

int main() {
    std::vector<uint8_t> guest(0x24a0000);
    std::array<uint8_t, 8192> dummy{};
    setup_guest_globals(guest, dummy);
    test_morton();
    test_quad();
    std::puts("game guest fastpaths: generated-body and oracle checks passed");
}
