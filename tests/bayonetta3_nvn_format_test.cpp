// Production format_block against an authentic table extracted by the offline audit.
#include "../runtime/src/nvn/nvn.h"
#undef NX_HLE
#define NX_HLE(name, function)
#include "../runtime/src/nvn/nvn_texture_layout.cpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace { std::vector<uint8_t> image(0xd38000); nx::LoadedModule module{}; }
namespace nx {
uint64_t find_symbol(const char*) { return module.base + 0x100; }
const LoadedModule* module_at(uint64_t) { return &module; }
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("requires authentic format table fixture");
        FILE* file = std::fopen(argv[1], "rb");
        if (!file) throw std::runtime_error("cannot open fixture");
        std::vector<uint8_t> table(0xd0 * 0x30);
        size_t count = std::fread(table.data(), 1, table.size(), file);
        std::fclose(file);
        if (count != table.size()) throw std::runtime_error("incomplete fixture");
        // Legacy table area stays zero: the inherited offset would return 1x1/4.
        std::memcpy(image.data() + 0xcd0d90, table.data(), table.size());
        module.base = reinterpret_cast<uint64_t>(image.data()); module.size = image.size();
        for (uint32_t format = 0; format < 0xd0; ++format) {
            const uint8_t* entry = table.data() + format * 0x30;
            uint32_t w = 1, h = 1, bytes = 4;
            if (entry[8] && entry[9] && entry[0x10]) { w = entry[8]; h = entry[9]; bytes = entry[0x10]; }
            uint32_t actual_w, actual_h, actual_bytes;
            nx::nvn::format_block(format, actual_w, actual_h, actual_bytes);
            if (actual_w != w || actual_h != h || actual_bytes != bytes)
                throw std::runtime_error("production format_block disagrees with original SDK table");
        }
        uint32_t w, h, bytes;
        nx::nvn::format_block(0x42, w, h, bytes);
        if (w != 4 || h != 4 || bytes != 8) throw std::runtime_error("BC1 must be 4x4/8");
        for (uint32_t format = 0x79; format <= 0x94; ++format) {
            nx::nvn::format_block(format, w, h, bytes);
            if (bytes != 16) throw std::runtime_error("ASTC must be 16 bytes per block");
        }
        std::puts("PASS production format_block: all 208 original SDK records; BC1 and all 28 ASTC formats");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL %s\n", error.what()); return 1; }
}
