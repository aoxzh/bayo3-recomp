#include "nvn_spirv_precision.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
using namespace nx::nvn;
int main(int argc, char** argv) {
    if (argc == 3) {
        std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
        if (!in || in.tellg() < 20 || in.tellg() % 4) return 2;
        std::vector<uint32_t> code(static_cast<size_t>(in.tellg()) / 4);
        in.seekg(0); in.read(reinterpret_cast<char*>(code.data()), code.size() * 4);
        if (!in || !precise_position(code)) return 3;
        auto once = code;
        if (!precise_position(code) || code != once) return 4;
        std::ofstream out(argv[2], std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * 4);
        return out ? 0 : 5;
    }
    const std::vector<uint32_t> header{0x07230203, 0x10000, 0, 100, 0};
    auto rejected = [](std::vector<uint32_t> code) {
        auto before = code;
        assert(!precise_position(code));
        assert(code == before);
    };
    rejected({});
    auto code = header; code.push_back(0); rejected(code);
    code = header; code.insert(code.end(), {(5u << 16) | 129u, 1, 2}); rejected(code);
    code = header; code.insert(code.end(), {(3u << 16) | 11u, 1, 0x41414141}); rejected(code);
    code = header; code.insert(code.end(), {(5u << 16) | 129u, 1, 2, 3, 4}); rejected(code); // no types
    code = header; code.insert(code.end(), {(2u << 16) | 19u, 1, (5u << 16) | 129u, 1, 100, 3, 4}); rejected(code);
}
