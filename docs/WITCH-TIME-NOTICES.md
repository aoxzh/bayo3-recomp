# Witch Time code and licensing notice

`runtime/src/b3_scene_clock.cpp` and its interface in
`runtime/include/nx/b3_scene_clock.h` adapt the fixed-30 scene-clock correction
from [Witch Time](https://github.com/Zexyen/Witch-Time), commit
`beffd6bd250d2da359b25a289cbb5bc5bccc4750`:

- [`runtime/scene_step.h`](https://github.com/Zexyen/Witch-Time/blob/beffd6bd250d2da359b25a289cbb5bc5bccc4750/runtime/scene_step.h)
- [`runtime/game_shim_eh.c`, fixed-step portion of `h_frame_clock`](https://github.com/Zexyen/Witch-Time/blob/beffd6bd250d2da359b25a289cbb5bc5bccc4750/runtime/game_shim_eh.c)

Witch Time's [upstream notice](https://github.com/Zexyen/Witch-Time/blob/beffd6bd250d2da359b25a289cbb5bc5bccc4750/NOTICE.md)
credits B2recomp, Copyright © 2026 K0NL, and the ssbu-recomp authors for the
parts identified there; it describes Witch Time as a Bayonetta 3 adaptation of
B2recomp. The upstream notice offers these files under the GNU General Public
License, version 3 or, at the recipient's option, any later version. The full
upstream GPL version 3 text is copied verbatim in `docs/LICENSE-GPL-3.0.txt`.
The adapted scene-clock and guest-fast-path files are marked
`SPDX-License-Identifier: GPL-3.0-or-later` and retain that licensing option.
They were adapted for this runtime on 2026-10-31.

This adaptation keeps the upstream ordinary fixed-30 debt smoothing, while
isolating debt per thread, resetting it on clock or mode changes and rejecting
invalid float samples. It does not copy the upstream one-pass or 60 FPS
branches. Only the exact environment value `NX_SCENE_TIME=0` disables the
correction; an unset, empty or other value leaves it enabled. The value is read
once when a fixed clock is first handled.

Since 2026-11-09 the same file also adapts the measured-step part of the uncap
branch of `h_frame_clock` at commit `7e7af36250b6845eb8a5ebd20744a9858c8e9d28`
([`runtime/game_shim_eh.c`](https://github.com/Zexyen/Witch-Time/blob/7e7af36250b6845eb8a5ebd20744a9858c8e9d28/runtime/game_shim_eh.c)):
above 60 Hz the gameplay step becomes this frame's milliseconds over the 60 Hz
frame's, floored at 1/16 with no carry, unless the game's own override is set.
Here it is applied when the runtime's display clock is above 60 Hz
(`NX_FPS_STEP=0` keeps the game's own step, `NX_FPS_STEP=1` applies it at any
rate), and it is followed by the frame counters below.

`runtime/src/b3_fps_counters.cpp`, its interface in
`runtime/include/nx/b3_fps_counters.h` and the `FPS_SITES` table in
`recomp/b3_hooks.py` derive from the same commit `7e7af36…`:

- [`runtime/game_shim_eh.c`, `fps_ci`, `fps_cf`, `fps_fs` and the frames-due publication in `h_frame_clock`](https://github.com/Zexyen/Witch-Time/blob/7e7af36250b6845eb8a5ebd20744a9858c8e9d28/runtime/game_shim_eh.c)
- [`tools/wtbuild/fps_counters.txt`, the counter store sites and what each counts](https://github.com/Zexyen/Witch-Time/blob/7e7af36250b6845eb8a5ebd20744a9858c8e9d28/tools/wtbuild/fps_counters.txt)

Witch Time's lifter (`tools/wtbuild/src/lifter/fps.rs`) is not used: the op
that feeds each listed store was read again in this recompiler's generated
code, and the generator rewrites only that op, after checking its address and
disassembly. The visit rules are reimplemented in C++ with one lock instead of
per-slot atomics, and add a `NX_FPS_COUNTERS_WATCH` measurement switch.
`NX_FPS_COUNTERS=0` keeps the stock counts. These files are marked
`SPDX-License-Identifier: GPL-3.0-or-later`.

`runtime/src/b3_guest_fastpaths.cpp` and its interface in
`runtime/include/nx/b3_guest_fastpaths.h` also derive from the same commit:

- [`runtime/game_shim_eh.c`, `even_bits16`, `h_morton_split32`, and `h_quad_walk`](https://github.com/Zexyen/Witch-Time/blob/beffd6bd250d2da359b25a289cbb5bc5bccc4750/runtime/game_shim_eh.c)
- [`runtime/quad_walk.h`, homogeneous-node classification](https://github.com/Zexyen/Witch-Time/blob/beffd6bd250d2da359b25a289cbb5bc5bccc4750/runtime/quad_walk.h)

The adaptation preserves the inclusive coordinates and width-based row stride,
adds overlap and integer-overflow fallback checks, and reproduces this
recompiler's guest ABI and condition flags. Mixed quadtree nodes execute the
original generated body. The generator selects these adapters only for the
verified Bayonetta 3 1.2.0 main NSO SHA-256
`499983f6adabc071488be7fe75f547c079c8c8dac95c3573e78a4277d7641800`.

The repository's original `LICENSE` remains the MIT license for its original
MIT-licensed code; this notice does not remove those copyright and permission
notices. A program distributed as a combined work with these GPL adaptations
must be conveyed in compliance with GPL-3.0-or-later, including its
applicable requirements for corresponding source, license notices and the
absence of warranty. Consult the included license text for its full terms.

`runtime/src/b3_model_lod.cpp`, its interface in `runtime/include/nx/b3_model_lod.h` and the `MODEL_LOD` entry
of `recomp/b3_hooks.py` follow `h_model_lod` (issue 86) in
[`runtime/game_shim_eh.c`](https://github.com/Zexyen/Witch-Time/blob/7e7af36250b6845eb8a5ebd20744a9858c8e9d28/runtime/game_shim_eh.c)
at the same commit: after the mesh level picker at main+0xBD563C, a mesh with more than one level keeps level 0.
Here it is a post-call wrapper of the generated function, switched by `model_lod=highest` and live from the settings
panel. It is marked `SPDX-License-Identifier: GPL-3.0-or-later`.
