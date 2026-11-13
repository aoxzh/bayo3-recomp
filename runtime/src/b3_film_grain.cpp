// SPDX-License-Identifier: GPL-3.0-or-later
// Film grain off (nx/b3_film_grain.h). Bayonetta 3 1.2.0 describes the fields of its scene parameter blocks with static
// descriptors, built on first use and chained into a list (next at +0x30) that the loader walks by name: name at +0,
// type at +8 (7: float), offset at +0x10. Four describe m_GrainIntensity at offset 0x1fc: two in the tonemap filter's
// table (0x2632810, 0x26329d0, the ones the "No Film Grain" mod clears) and two in a second table built the same way
// (0x26343a0, 0x2634560). Pointing their name two bytes further ("GrainIntensity") makes them match no key in the
// scene files, which is what dropping their registration does, without touching the list.
#include "nx/b3_film_grain.h"

#include <cstring>

#include <nx/runtime.h>  // kModuleBase, log

#include "platform.h"

namespace nx::b3 {

namespace {
constexpr uint64_t kName = 0x177337d;  // "m_GrainIntensity" in main's .rodata (1.2.0)
constexpr uint64_t kDescriptors[] = {0x2632810, 0x26329d0, 0x26343a0, 0x2634560};
bool g_checked, g_ours;  // the string is the one this table was made for
int g_state = -1;        // what the descriptors were last set to: 1 off, 0 on
}  // namespace

void film_grain_frame() {
    const int off = plat::film_grain_off();
    if (!g_checked) {
        g_checked = true;
        g_ours = std::memcmp(reinterpret_cast<const void*>(std::uintptr_t(kModuleBase + kName)), "m_GrainIntensity", 17) == 0;
        if (!g_ours) log("film grain: m_GrainIntensity is not where 1.2.0 has it; film_grain = off does nothing");
    }
    if (!g_ours || (off == g_state && !off)) return;
    // The descriptors are filled lazily (a guarded static), so while off keep looking for new ones.
    const uint64_t game = kModuleBase + kName, renamed = game + 2;
    int changed = 0;
    for (uint64_t d : kDescriptors) {
        auto* name = reinterpret_cast<uint64_t*>(std::uintptr_t(kModuleBase + d));
        uint32_t type, field;
        std::memcpy(&type, reinterpret_cast<const void*>(std::uintptr_t(kModuleBase + d + 8)), 4);
        std::memcpy(&field, reinterpret_cast<const void*>(std::uintptr_t(kModuleBase + d + 0x10)), 4);
        if (type != 7 || field != 0x1fc) continue;  // not built yet
        if (off && *name == game) { *name = renamed; ++changed; }
        else if (!off && *name == renamed) { *name = game; ++changed; }
    }
    if (changed) log("film grain: %s (%d field descriptors)", off ? "off from the next scene" : "the game's own again", changed);
    g_state = off;
}

}  // namespace nx::b3
