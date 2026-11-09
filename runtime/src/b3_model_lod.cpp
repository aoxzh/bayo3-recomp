// SPDX-License-Identifier: GPL-3.0-or-later
// Model detail (nx/b3_model_lod.h). Follows Witch Time runtime/game_shim_eh.c h_model_lod at
// 7e7af36250b6845eb8a5ebd20744a9858c8e9d28; see docs/WITCH-TIME-NOTICES.md.
#include "nx/b3_model_lod.h"

#include <atomic>
#include <cstring>

#include <nx/runtime.h>  // log

#include "platform.h"

namespace nx::b3 {

void model_lod_after(uint64_t mesh, uint64_t out, int32_t limit) {
    if (!plat::model_lod() || !mesh || !out) return;
    int32_t last, level;
    std::memcpy(&last, reinterpret_cast<const void*>(std::uintptr_t(mesh + 0x14)), sizeof(last));
    std::memcpy(&level, reinterpret_cast<const void*>(std::uintptr_t(out)), sizeof(level));
    const int32_t levels = limit < last ? limit : last;  // the last level this call could pick
    // A coarser level, or hidden past the last distance of a mesh that has more than one level: the finest instead.
    // Meshes with a single level keep their own distance cut, as the Switch "Increased LOD" mod does.
    if (level > 0 || (level == -1 && levels > 0)) {
        const int32_t finest = 0;
        std::memcpy(reinterpret_cast<void*>(std::uintptr_t(out)), &finest, sizeof(finest));
        static std::atomic<uint64_t> raised{0};
        const uint64_t n = raised.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n == 100000 || n == 10000000)
            log("model detail: highest: %llu picks raised to the finest level (this one was %d of %d)",
                static_cast<unsigned long long>(n), level, levels);
    }
}

}  // namespace nx::b3
