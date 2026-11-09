// SPDX-License-Identifier: GPL-3.0-or-later
// Model detail. Follows Witch Time runtime/game_shim_eh.c h_model_lod at 7e7af36250b6845eb8a5ebd20744a9858c8e9d28
// (issue 86); see docs/WITCH-TIME-NOTICES.md.
#pragma once

#include <cstdint>

namespace nx::b3 {

// Call after main+0xBD563C (the mesh level picker) with its entry x0 (the mesh's level table), x1 (where it writes
// the level: -2 none, -1 hidden, 0 finest) and w2 (the last level the caller allows). With model_lod = highest
// (b3.conf; the settings panel changes it live) a mesh with more than one level keeps level 0 at any distance.
void model_lod_after(uint64_t mesh, uint64_t out, int32_t limit);

}  // namespace nx::b3
