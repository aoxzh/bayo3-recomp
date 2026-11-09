// Player settings that change while the game runs (platform.h "live settings"): read at start from the player's
// settings (NX_<KEY> on PC, config.txt on Android), changed by the settings panel, read by the game hooks.
#include <atomic>
#include <cstring>
#include <string>

#include "platform.h"

namespace nx::plat {

namespace {
std::atomic<int> g_model_lod{-1};  // -1 not read yet
}

int model_lod() {
    int v = g_model_lod.load(std::memory_order_relaxed);
    if (v < 0) {
        v = setting("model_lod") == "highest" ? 1 : 0;
        g_model_lod.store(v, std::memory_order_relaxed);
    }
    return v;
}

void set_model_lod(int v) { g_model_lod.store(v ? 1 : 0, std::memory_order_relaxed); }

}  // namespace nx::plat
