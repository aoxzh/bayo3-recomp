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

// The display clock the runtime paces presents by (hle_vi.cpp). Above 60 the measured (gameplay) step is replaced
// by measured_step(); NX_FPS_STEP=0 keeps the game's own, NX_FPS_STEP=1 applies it at any rate.
void scene_clock_set_display_hz(int hz);

// This frame's step in 60 Hz frames: ms / per, clamped to [1/16, 2]. -1 when the game's own value must stand
// (its override is set, or an input is not a usable time).
float measured_step(float ms, float per, float override_step);

}  // namespace nx::b3
