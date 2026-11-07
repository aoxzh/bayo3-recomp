// B3 offline cache verification against the actual SMW runtime contract.
// Build only with NX_SHADERS_STANDALONE; does not execute game code or use Vulkan.
#include "nvn_shaders.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <map>
using namespace nx::nvn;
namespace fs = std::filesystem;
static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("missing " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::vector<std::string> split(std::string s) {
    std::vector<std::string> out; std::istringstream in(s); std::string c;
    while (std::getline(in, c, '\t')) out.push_back(c);
    return out;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        fs::path root = argv[1], src = root / "shaders_extracted/root_000";
        std::unordered_map<std::string, std::string> resources;
        parse_results_tsv(read(root / "spirv/results.tsv"), resources);
        std::istringstream manifest(read(src / "manifest.tsv")); std::string line;
        std::getline(manifest, line);
        size_t checked = 0, skipped = 0, failed = 0, compute = 0, bindings = 0;
        std::map<std::string, size_t> kinds;
        while (std::getline(manifest, line)) {
            auto c = split(line); if (c.size() != 13) throw std::runtime_error("bad manifest");
            auto it = resources.find(c[10]); if (it == resources.end()) { skipped++; continue; }
            auto code = read(src / c[11]), control = read(src / c[12]);
            auto bytes = read(root / "spirv" / (c[10] + ".spv"));
            std::string error;
            std::vector<uint32_t> spv(bytes.size()/4);
            std::memcpy(spv.data(), bytes.data(), bytes.size());
            ShaderInfo info;
            bool ok = control.size() >= 0x758 && bytes.size()%4 == 0 &&
                make_shader_info(reinterpret_cast<const uint8_t*>(code.data()),
                                 reinterpret_cast<const uint8_t*>(control.data()),
                                 std::move(spv), &it->second, info, &error);
            if (ok && info.hash != c[10]) ok = false, error = "runtime hash " + info.hash;
            // The extraction manifest spells the tessellation stages tess-control/tess-evaluation (bayonetta3_shaders.py
            // STAGES); the runtime's stage_name() says tessellationcontrol/tessellationevaluation.
            const std::string manifest_stage = c[1] == "tess-control"      ? "tessellationcontrol"
                                               : c[1] == "tess-evaluation" ? "tessellationevaluation"
                                                                           : c[1];
            if (ok && stage_name(info.stage) != manifest_stage)
                ok = false, error = std::string("stage ") + stage_name(info.stage) + " vs manifest " + c[1];
            if (ok && info.control.code_size != code.size())
                ok = false, error = "control code size " + std::to_string(info.control.code_size) + " vs " +
                                    std::to_string(code.size()) + " extracted bytes";
            if (ok && info.stage == ShaderStage::Compute) {
                compute++;
                for (int i = 0; i < 3; i++)
                    if (info.local_size[i] != info.refl.local_size[i])
                        ok = false, error = "compute local size " + std::to_string(i) + ": control " +
                                            std::to_string(info.local_size[i]) + ", SPIR-V " +
                                            std::to_string(info.refl.local_size[i]);
            }
            if (!ok) { std::cout << "FAIL\t" << c[10] << '\t' << error << '\n'; failed++; }
            else { checked++; kinds[c[1]]++; bindings += info.refl.bindings.size(); }
        }
        std::cout << "checked=" << checked << " failed=" << failed << " skipped=" << skipped
                  << " records=" << resources.size() << " compute=" << compute << " bindings=" << bindings << '\n';
        for (const auto& [stage,n] : kinds) std::cout << stage << '=' << n << '\n';
        return failed || checked != resources.size() || !checked ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
