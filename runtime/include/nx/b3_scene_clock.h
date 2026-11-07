// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from Witch Time runtime/scene_step.h and game_shim_eh.c at
// beffd6bd250d2da359b25a289cbb5bc5bccc4750. See docs/WITCH-TIME-NOTICES.md.
#pragma once

#include <cstdint>

namespace nx::b3 {

// Call after the original game clock update, passing its entry x0 and whether
// its entry w1 was zero. The guest clock must remain valid for the call.
// NX_SCENE_TIME=0 disables the correction; the setting is read once per process.
void scene_clock_after(uint64_t clock, bool fixed);

}  // namespace nx::b3
