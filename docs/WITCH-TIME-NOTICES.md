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
upstream GPL version 3 text is in the root `LICENSE` and mirrored in
`docs/LICENSE-GPL-3.0.txt`.
The adapted scene-clock and guest-fast-path files are marked
`SPDX-License-Identifier: GPL-3.0-or-later` and retain that licensing option.
They were adapted for this runtime on 2026-10-31.

This adaptation keeps the upstream ordinary fixed-30 debt smoothing, while
isolating debt per thread, resetting it on clock or mode changes and rejecting
invalid float samples. It leaves the game's measured-step path unchanged and
does not copy
the upstream one-pass, 60 FPS or uncap branches. Only the exact environment
value `NX_SCENE_TIME=0` disables the correction; an unset, empty or other value
leaves it enabled. The value is read once when a fixed clock is first handled.

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

The original MIT terms and copyright notice are preserved in `LICENSE-MIT`;
this notice does not remove those copyright and permission notices. The root
`LICENSE` is the GNU GPL version 3 text for the combined work, and `NOTICE`
summarizes the layout. A program distributed as a combined work with these
GPL adaptations must be conveyed in compliance with GPL-3.0-or-later,
including its applicable requirements for corresponding source, license
notices and the absence of warranty. Consult the included license text for
its full terms.
