// Focused regression for the real frame-source capture helper. Synthetic memory
// and callbacks model the caller; an optional analyzed main verifies the fixture.
#include "../runtime/src/movie_frame_source.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <thread>
#include <vector>

namespace fs = nx::movie_frame_source;
constexpr uint64_t base = 0x10000000;
constexpr uint64_t controller = 0x50000000;
constexpr uint64_t planes = 0x60000000;
constexpr uint64_t decoder = 0x70000000;
constexpr uint64_t file = 0x100000037ull;
constexpr int64_t pts = 0x123456789ll;
constexpr uint64_t y = 0x700000001ull, uv = 0x700100002ull;

struct Event { uint64_t file; int64_t pts; uint64_t y, uv; };
struct Memory {
    std::map<uint64_t, std::vector<uint8_t>> regions;
    uint64_t deny = 0;
    bool read(uint64_t addr, void* out, size_t len) const {
        if (deny && addr <= deny && deny - addr < len) return false;
        auto i = regions.upper_bound(addr);
        if (i == regions.begin()) return false;
        --i;
        const uint64_t delta = addr - i->first;
        if (delta > i->second.size() || len > i->second.size() - size_t(delta)) return false;
        std::memcpy(out, i->second.data() + size_t(delta), len);
        return true;
    }
    template <class T> void put(uint64_t addr, T value) {
        auto i = regions.upper_bound(addr);
        assert(i != regions.begin());
        --i;
        const uint64_t delta = addr - i->first;
        assert(delta <= i->second.size() && sizeof(value) <= i->second.size() - size_t(delta));
        std::memcpy(i->second.data() + size_t(delta), &value, sizeof(value));
    }
    void flip(uint64_t addr) {
        auto i = regions.upper_bound(addr);
        assert(i != regions.begin());
        --i;
        assert(addr - i->first < i->second.size());
        i->second[size_t(addr - i->first)] ^= 1;
    }
};

struct Fixture {
    Memory mem;
    std::vector<Event> events;
    Fixture() {
        for (const auto& part : fs::kCodeParts)
            mem.regions[base + part.offset] = std::vector<uint8_t>(part.bytes, part.bytes + part.size);
        mem.regions[controller] = std::vector<uint8_t>(0x338);
        mem.regions[planes] = std::vector<uint8_t>(16);
        mem.put<uint32_t>(controller + 0x320, 3);
        mem.put<uint64_t>(controller + 0x160, decoder);
        mem.put<uint32_t>(controller + 0x328, 7);
        mem.put<uint32_t>(controller + 0x10, 1);
        mem.put<uint64_t>(controller + 0x18, file);
        mem.put<int64_t>(controller + 0x330, pts);
        mem.put<uint64_t>(planes, y);
        mem.put<uint64_t>(planes + 8, uv);
    }
    fs::Capture begin(bool match = true, uint64_t ctl = controller, uint64_t dec = decoder,
                      uint64_t idx = 7, uint64_t p = planes, uint64_t module = base) {
        auto read = [this](uint64_t a, void* o, size_t n) { return mem.read(a, o, n); };
        auto publish = [this](uint64_t h, int64_t t, uint64_t l, uint64_t c) { events.push_back({h,t,l,c}); };
        return fs::begin(match, module, ctl, dec, idx, p, read, publish);
    }
    void finish(const fs::Capture& capture, bool real = true, uint64_t result = 0) {
        auto publish = [this](uint64_t h, int64_t t, uint64_t l, uint64_t c) { events.push_back({h,t,l,c}); };
        fs::finish(capture, real, result, publish);
    }
    void clear_only(const fs::Capture& c) {
        finish(c);
        assert(!c.valid && events.size() == 1);
        assert(events[0].file == 0 && events[0].pts == -1 && events[0].y == 0 && events[0].uv == 0);
    }
};

void reference(const char* path) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    for (const auto& part : fs::kCodeParts) {
        std::array<uint8_t,64> bytes{};
        f.seekg(std::streamoff(part.offset));
        f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(part.size));
        assert(f.gcount() == std::streamsize(part.size));
        assert(std::memcmp(bytes.data(), part.bytes, part.size) == 0);
    }
}

void checks() {
    assert(fs::matching_caller(base, base + fs::kReturnOffset));
    assert(!fs::matching_caller(base, base + fs::kReturnOffset + 4));
    assert(!fs::matching_caller(UINT64_MAX - 3, 0));
    {
        Fixture f;
        auto c = f.begin(false);
        f.finish(c);
        assert(!c.valid && f.events.empty()); // another caller cannot clear a pending frame
    }
    {
        Fixture f;
        auto c = f.begin();
        assert(c.valid && f.events.size() == 1 && f.events[0].file == 0 && f.events[0].pts == -1);
        // The real call is allowed to clobber guest memory: the saved values
        // refer to the selected frame before entering that call.
        f.mem.put<int64_t>(controller + 0x330, 123);
        f.mem.put<uint64_t>(planes, 456);
        f.finish(c);
        assert(f.events.size() == 2);
        assert(f.events[1].file == file && f.events[1].pts == pts && f.events[1].y == y && f.events[1].uv == uv);
    }
    for (bool real : {false, true}) {
        Fixture f;
        auto c = f.begin();
        assert(c.valid);
        f.finish(c, real, 0x100c);
        assert(f.events.size() == 1); // null or failed real call cannot publish
    }
    {
        Fixture f;
        auto c = f.begin();
        f.finish(c, false, 0);
        assert(f.events.size() == 1); // null vtable slot returns a synthetic zero
    }
    {
        Fixture f;
        auto c = f.begin(true, controller, decoder, 0x100000007ull);
        f.finish(c, true, 0x100000000ull);
        assert(f.events.size() == 2); // guest compares w1 and w0, respectively
    }
    for (const auto& part : fs::kCodeParts) {
        for (size_t i = 0; i < part.size; ++i) {
            Fixture f;
            f.mem.flip(base + part.offset + i);
            f.clear_only(f.begin()); // every individual fingerprint byte matters
        }
        Fixture f;
        f.mem.deny = base + part.offset + part.size - 1;
        f.clear_only(f.begin());
    }
    for (uint64_t addr : {controller + 0x320, controller + 0x160, controller + 0x328,
                          controller + 0x10, controller + 0x18, controller + 0x330,
                          planes, planes + 15}) {
        Fixture f;
        f.mem.deny = addr;
        f.clear_only(f.begin());
    }
    {
        Fixture f; f.mem.put<uint32_t>(controller + 0x320, 2); f.clear_only(f.begin());
    }
    {
        Fixture f; f.mem.put<uint64_t>(controller + 0x160, decoder + 1); f.clear_only(f.begin());
    }
    {
        Fixture f; f.mem.put<uint32_t>(controller + 0x328, 8); f.clear_only(f.begin());
    }
    {
        Fixture f; f.mem.put<uint32_t>(controller + 0x10, 0); f.clear_only(f.begin());
    }
    {
        Fixture f; f.mem.put<uint64_t>(controller + 0x18, 0); f.clear_only(f.begin());
    }
    {
        Fixture f; f.mem.put<int64_t>(controller + 0x330, -1); f.clear_only(f.begin());
    }
    for (uint64_t off : {0ull, 8ull}) {
        Fixture f; f.mem.put<uint64_t>(planes + off, 0); f.clear_only(f.begin());
    }
    {
        Fixture f; f.clear_only(f.begin(true, 0));
    }
    {
        Fixture f; f.clear_only(f.begin(true, UINT64_MAX - 2));
    }
    {
        Fixture f; f.clear_only(f.begin(true, controller, 0));
    }
    {
        Fixture f; f.clear_only(f.begin(true, controller, decoder, 7, UINT64_MAX - 2));
    }
    {
        Fixture f; f.clear_only(f.begin(true, controller, decoder, 7, planes, UINT64_MAX - 2));
    }
    {
        Fixture f; auto c = f.begin(); f.finish(c);
        f.mem.put<uint32_t>(controller + 0x320, 0);
        c = f.begin(); f.finish(c);
        assert(f.events.size() == 3 && f.events[2].file == 0 && f.events[2].pts == -1);
    }
}

int main(int argc, char** argv) {
    if (argc == 2) reference(argv[1]);
    else assert(argc == 1);
    checks();
    std::thread a(checks), b(checks);
    a.join(); b.join();
    std::cout << "movie frame source: valid, rejected, fingerprint, lifecycle and concurrency ok\n";
}
