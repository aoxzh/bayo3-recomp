// SPDX-License-Identifier: GPL-3.0-or-later
// Frame counters above 60 Hz. The rules follow Witch Time runtime/game_shim_eh.c fps_ci / fps_cf / fps_fs at
// 7e7af36250b6845eb8a5ebd20744a9858c8e9d28 (from B2recomp's fps_patch.py); see docs/WITCH-TIME-NOTICES.md.
#pragma once

#include <cstdint>

namespace nx::b3 {

// Bayonetta 3 counts many windows in logic steps: one a frame, made for 60 frames a second. Above 60 Hz the
// generated code (recomp/b3_hooks.py FPS_SITES) asks these how far a counter moves at this visit instead of 1:
// by the 60 Hz frames of game time due since its last visit, so a 15-frame window lasts 0.25 s at 60, 90 or 120.
// Off (the stock amount) unless the clock publishes with enabled = true; NX_FPS_COUNTERS=0 keeps it off.

// key: the counter's store site; field: the counter's address. before: its value before the op; k: the immediate.
// sign: -1 a countdown, +1 an up-counter. Returns the amount to subtract or add (k times the frames due).
uint32_t fps_ci(uint64_t key, uint64_t field, uint32_t before, int k, int sign);
// A float countdown's -1.0: the number of frames that multiplies it.
int fps_cf(uint64_t key, uint64_t field, float before, int sign);
// A float that moves by a per-frame amount: the factor of that amount (fractional frames of game time).
float fps_fs(uint64_t key, uint64_t field);

// Called once per game clock update (b3_scene_clock.cpp) with the step the frame will run with, in 60 Hz frames.
void fps_counters_publish(float step, bool enabled);

// Test hook: forget every counter and update.
void fps_counters_reset();

}  // namespace nx::b3
