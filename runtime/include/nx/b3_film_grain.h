// SPDX-License-Identifier: GPL-3.0-or-later
// Film grain off (film_grain = off). The finding is Witch Time's (docs/STATUS.md, issue 86, at
// 58499b8cf87c4be83ff92fc8897bb8f7f5b3bb97; docs/WITCH-TIME-NOTICES.md): the grain is the scene parameter
// m_GrainIntensity (offset 0x1fc of the tonemap filter's parameters), and the community "No Film Grain" mod removes that
// field's registration so the scene files never set it. Witch Time edits its own shader translation instead.
#pragma once

#include <cstdint>

namespace nx::b3 {

// Once per present: while film_grain = off, the field descriptors the scene-parameter loader matches by name carry
// another name, so the loader leaves the grain at its default (none) from the next scene on. Restored when it is on.
void film_grain_frame();

}  // namespace nx::b3
