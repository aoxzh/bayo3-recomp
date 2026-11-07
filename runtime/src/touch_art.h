// The touch controls' art (touch_art.cpp): one premultiplied RGBA8 atlas, drawn once on the CPU, from which the
// present pass (nvn_present.cpp) draws sprites. The look is a translucent light "Joy-Con silver" -- the material the
// Switch emulators' Android overlays settled on -- drawn here from distance functions (no image files): bevelled discs
// lit from the upper left with dark labels, a domed stick thumb with a black rim and a concave groove over a recessed
// range ring, a rounded-cross D-pad with arrow marks, stadium shoulders and trigger-shaped ZL / ZR. A pressed piece
// turns dark and its label white. Labels are drawn white and tinted by the sprite.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nx::plat::art {

enum Id : int {
    kStickBase, kStickBaseActive, kStickThumb, kStickThumbActive,
    kDisc, kDiscHeld, kPill, kPillHeld,
    kTriggerL, kTriggerLHeld, kTriggerR, kTriggerRHeld,
    kDpad, kDpadUp, kDpadRight, kDpadDown, kDpadLeft, kRing,
    kGlyphA, kGlyphB, kGlyphX, kGlyphY, kGlyphL, kGlyphR, kGlyphPlus, kGlyphMinus, kGlyphZL, kGlyphZR,
    kCount
};

struct Cell {
    uint16_t x, y, w, h;  // pixels in the atlas
};

constexpr uint32_t kAtlasW = 2048, kAtlasH = 1280;

const Cell& cell(int id);
// kAtlasW * kAtlasH * 4 bytes, premultiplied RGBA8: drawn on the first call (about a second on a phone core), then
// kept. atlas_ready() says whether that has happened, so a frame need not wait for it (prepare_atlas() starts it on a
// background thread).
const uint8_t* atlas();
bool atlas_ready();
void prepare_atlas();

}  // namespace nx::plat::art
