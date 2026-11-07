// The touch controls' atlas (touch_art.h). Every piece is a signed distance function shaded with one pixel of
// anti-aliasing, so it stays smooth at the sizes the present pass scales it to (a 2176x1812 foldable's inner screen draws the stick
// base at about 470 px, its cell is 512). Coordinates inside a cell: the shorter half-extent is 1, y points down.
#include "touch_art.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace nx::plat::art {

namespace {

constexpr float kPi = 3.14159265f;

// x, y, w, h.
constexpr Cell kCells[kCount] = {
    {0, 0, 512, 512},      {512, 0, 512, 512},     // stick range, at rest / held
    {1024, 0, 256, 256},   {1280, 0, 256, 256},    // stick thumb
    {1536, 0, 256, 256},   {1792, 0, 256, 256},    // round button
    {1024, 256, 384, 192}, {1408, 256, 384, 192},  // shoulder (stadium)
    {0, 1024, 320, 192},   {320, 1024, 320, 192},  {640, 1024, 320, 192}, {960, 1024, 320, 192},  // ZL, ZR
    {0, 512, 384, 384},    {384, 512, 384, 384},   {768, 512, 384, 384},  {1152, 512, 384, 384},
    {1536, 512, 384, 384},                                                 // D-pad and its four lit arms
    {1792, 256, 256, 256},                                                 // the press ripple
    {0, 896, 128, 128},    {128, 896, 128, 128},   {256, 896, 128, 128},  {384, 896, 128, 128},  // A B X Y
    {512, 896, 128, 128},  {640, 896, 128, 128},   {768, 896, 128, 128},  {896, 896, 128, 128},  // L R + -
    {1024, 896, 256, 128}, {1280, 896, 256, 128},                                                // ZL ZR labels
};

struct Rgba {
    float r, g, b, a;  // straight alpha
};

struct Canvas {
    std::vector<float> px;  // premultiplied, kAtlasW * kAtlasH * 4
    Canvas() : px(size_t(kAtlasW) * kAtlasH * 4, 0.0f) {}
    // Composite `paint(u, v, pixel)` (straight colour, coverage folded into alpha) over the cell.
    template <class F>
    void layer(const Cell& c, F paint) {
        const float half = float(std::min(c.w, c.h)) * 0.5f, pixel = 1.0f / half;
        for (uint32_t y = 0; y < c.h; y++)
            for (uint32_t x = 0; x < c.w; x++) {
                const float u = (float(x) + 0.5f - float(c.w) * 0.5f) / half;
                const float v = (float(y) + 0.5f - float(c.h) * 0.5f) / half;
                const Rgba s = paint(u, v, pixel);
                if (!(s.a > 0.0f)) continue;
                float* d = &px[(size_t(c.y + y) * kAtlasW + c.x + x) * 4];
                const float k = 1.0f - s.a;
                d[0] = s.r * s.a + d[0] * k, d[1] = s.g * s.a + d[1] * k, d[2] = s.b * s.a + d[2] * k;
                d[3] = s.a + d[3] * k;
            }
    }
};

float clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float smooth(float e0, float e1, float x) {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
// Coverage of a shape whose signed distance is d (negative inside), with a one pixel edge.
float cover(float d, float pixel) { return clamp01(0.5f - d / pixel); }
// Coverage of a line of half-width w at distance d.
float line(float d, float w, float pixel) { return cover(std::fabs(d) - w, pixel); }
Rgba grey(float g, float a) { return {g, g, g * 1.01f, a}; }

float seg(float px, float py, float ax, float ay, float bx, float by) {
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const float t = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy));
    return std::hypot(wx - vx * t, wy - vy * t);
}

// Distance to the arc of radius r around (cx, cy) between angles a0 < a1 (radians, y down).
float arc(float px, float py, float cx, float cy, float r, float a0, float a1) {
    float a = std::atan2(py - cy, px - cx);
    while (a < a0) a += 2 * kPi;
    if (a <= a1) return std::fabs(std::hypot(px - cx, py - cy) - r);
    return std::min(std::hypot(px - (cx + r * std::cos(a0)), py - (cy + r * std::sin(a0))),
                    std::hypot(px - (cx + r * std::cos(a1)), py - (cy + r * std::sin(a1))));
}

// A box of half extents (bx, by) with its corners rounded by tl, tr, br, bl (y down).
float round_box(float u, float v, float bx, float by, float tl, float tr, float br, float bl) {
    const float r = u < 0 ? (v < 0 ? tl : bl) : (v < 0 ? tr : br);
    const float qx = std::fabs(u) - bx + r, qy = std::fabs(v) - by + r;
    return std::min(std::max(qx, qy), 0.0f) + std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) - r;
}

// A filled triangle pointing along +x with its tip at `tip`, half-height h, length l (signed distance, approx.).
float arrow(float x, float y, float tip, float h, float l) {
    const float bx = tip - l;
    const float d_back = bx - x;                                    // behind the base
    const float nx = h, ny = l, n = std::hypot(nx, ny);             // the slanted edges' normal
    const float d_edge = ((x - tip) * nx + std::fabs(y) * ny) / n;  // the two slanted edges
    return std::max(d_back, d_edge);
}

// ---- the material ---------------------------------------------------------------------------------------------------

// Translucent silver over a signed distance `d` (negative inside, in cell units), lit from the upper left. `depth`
// is how far in from the edge the bevel reaches. Held: a dark, flat face.
Rgba silver(float u, float v, float d, float pixel, bool held, float depth = 0.12f) {
    const float c = cover(d, pixel);
    if (c <= 0.0f) return {0, 0, 0, 0};
    const float lit = clamp01(std::hypot(u + 0.45f, v + 0.55f) / 2.2f);  // 0 at the light, 1 far from it
    const float in = clamp01(-d / depth);                                // 0 at the edge, 1 past the bevel
    float g, a;
    if (!held) {
        g = lerp(0.95f, 0.72f, lit);
        g = lerp(g * 0.62f, g, smooth(0.0f, 1.0f, in));  // the bevel darkens towards the rim
        a = 0.58f;
    } else {
        g = lerp(0.34f, 0.20f, lit);
        g = lerp(g * 1.5f, g, smooth(0.0f, 1.0f, in));
        a = 0.74f;
    }
    return grey(g, c * a);
}

// The rim: a thin darker outline just inside the edge, and (not held) a light line along the upper inside edge.
Rgba rim(float u, float v, float d, float pixel, bool held) {
    const float outline = line(d + pixel * 0.8f, pixel * 0.9f, pixel) * (held ? 0.65f : 0.55f);
    float hi = 0.0f;
    if (!held) hi = line(d + 0.055f, 0.012f, pixel) * clamp01(-(v + 0.1f * u) * 1.6f) * 0.55f;
    if (hi > outline) return grey(1.0f, hi);
    return grey(held ? 0.80f : 0.18f, outline);
}

template <class Sd>
void draw_body(Canvas& cv, int id, bool held, Sd sd, float depth = 0.12f) {
    cv.layer(kCells[id], [&](float u, float v, float pixel) { return silver(u, v, sd(u, v), pixel, held, depth); });
    cv.layer(kCells[id], [&](float u, float v, float pixel) { return rim(u, v, sd(u, v), pixel, held); });
}

// ---- the sticks -----------------------------------------------------------------------------------------------------

// The range: a recessed well, clear in the middle and darker towards a raised light ring, with four small arrow marks.
void draw_range(Canvas& cv, int id, bool active) {
    const float k = active ? 1.35f : 1.0f;
    cv.layer(kCells[id], [&](float u, float v, float pixel) {
        const float r = std::hypot(u, v);
        const float c = cover(r - 0.97f, pixel);
        const float a = lerp(0.06f, 0.30f, smooth(0.0f, 0.86f, r)) * k;  // the well
        return Rgba{0.10f, 0.10f, 0.11f, c * std::min(a, 0.9f)};
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // the raised ring between 0.86 and 0.97
        const float r = std::hypot(u, v);
        const float d = std::fabs(r - 0.915f) - 0.055f;
        const float lit = clamp01(-(u + v) * 0.5f + 0.5f);
        return grey(lerp(0.55f, 0.88f, lit), cover(d, pixel) * 0.55f * std::min(k, 1.2f));
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // its edges
        const float r = std::hypot(u, v);
        const float a = std::max(line(r - 0.97f, pixel * 0.9f, pixel) * 0.55f, line(r - 0.86f, pixel * 0.8f, pixel) * 0.35f);
        return grey(0.15f, a);
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // arrow marks just inside the ring
        float d = 1e9f;
        for (int i = 0; i < 4; i++) {
            const float ca = std::cos(i * kPi / 2), sa = std::sin(i * kPi / 2);
            d = std::min(d, arrow(u * ca + v * sa, -u * sa + v * ca, 0.80f, 0.055f, 0.08f));
        }
        return grey(0.93f, cover(d, pixel) * (active ? 0.85f : 0.60f));
    });
}

// The thumb: a black rubber rim around a light dome with a concave groove and a soft highlight.
void draw_thumb(Canvas& cv, int id, bool active) {
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // a soft shadow under it
        const float r = std::hypot(u, v - 0.05f);
        return Rgba{0, 0, 0, (1.0f - smooth(0.80f, 1.0f, r)) * 0.35f};
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // the rim
        const float r = std::hypot(u, v);
        const float lit = clamp01(-(u + v) * 0.35f + 0.5f);
        const float g = lerp(0.05f, 0.24f, lit * smooth(0.92f, 0.84f, r));
        return grey(g, cover(r - 0.92f, pixel) * (active ? 0.92f : 0.80f));
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // the dome
        const float r = std::hypot(u, v);
        const float lit = clamp01(std::hypot(u + 0.35f, v + 0.42f) / 1.4f);
        float g = lerp(0.97f, 0.58f, lit);
        g *= lerp(1.0f, 0.82f, smooth(0.50f, 0.72f, r));  // the dome falls away towards the rim
        return grey(active ? std::min(1.0f, g * 1.04f) : g, cover(r - 0.72f, pixel) * (active ? 0.92f : 0.80f));
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // the concave groove and its light lower lip
        const float r = std::hypot(u, v);
        const float groove = line(r - 0.50f, 0.018f, pixel) * 0.30f;
        const float lip = line(r - 0.53f, 0.010f, pixel) * clamp01((u + v) * 0.6f + 0.3f) * 0.45f;
        return lip > groove ? grey(1.0f, lip) : grey(0.25f, groove);
    });
    cv.layer(kCells[id], [&](float u, float v, float pixel) {  // the highlight
        const float r = std::hypot(u + 0.26f, (v + 0.30f) * 1.25f);
        return grey(1.0f, (1.0f - smooth(0.0f, 0.24f, r)) * 0.45f);
    });
}

// ---- the D-pad ------------------------------------------------------------------------------------------------------

float cross(float u, float v) {
    const float w = 0.34f, l = 0.96f, r = 0.12f;
    return std::min(round_box(u, v, l, w, r, r, r, r), round_box(u, v, w, l, r, r, r, r));
}

// The arrow marks, one per arm; `only` limits them to one arm (0 up, 1 right, 2 down, 3 left), -1 all.
float dpad_arrows(float u, float v, int only) {
    float d = 1e9f;
    for (int i = 0; i < 4; i++) {
        if (only >= 0 && i != only) continue;
        const float ang = (i - 1) * kPi / 2;  // 0 up (-pi/2), 1 right (0), 2 down, 3 left
        const float ca = std::cos(ang), sa = std::sin(ang);
        d = std::min(d, arrow(u * ca + v * sa, -u * sa + v * ca, 0.80f, 0.12f, 0.17f));
    }
    return d;
}

void draw_dpad(Canvas& cv) {
    draw_body(cv, kDpad, false, cross, 0.10f);
    cv.layer(kCells[kDpad], [&](float u, float v, float pixel) {  // the shallow centre dimple
        const float r = std::hypot(u, v);
        return grey(0.30f, line(r - 0.17f, 0.012f, pixel) * 0.25f);
    });
    cv.layer(kCells[kDpad], [&](float u, float v, float pixel) {
        return grey(0.16f, cover(dpad_arrows(u, v, -1), pixel) * 0.78f);
    });
    // A lit arm: the arm (the cross beyond the centre square, towards it) darkened, its arrow white.
    for (int i = 0; i < 4; i++) {
        const int id = kDpadUp + i;
        auto in_arm = [i](float u, float v) {
            const float ang = (i - 1) * kPi / 2;
            const float along = u * std::cos(ang) + v * std::sin(ang);
            return std::max(cross(u, v), 0.30f - along);
        };
        cv.layer(kCells[id], [&](float u, float v, float pixel) {
            return Rgba{0.10f, 0.10f, 0.11f, cover(in_arm(u, v), pixel) * 0.62f};
        });
        cv.layer(kCells[id], [&](float u, float v, float pixel) {
            return grey(1.0f, cover(dpad_arrows(u, v, i), pixel) * 0.95f);
        });
    }
}

// ---- the labels -----------------------------------------------------------------------------------------------------

struct Stroke {
    bool is_arc;
    float a, b, c, d;  // segment: a,b -> c,d; arc: centre a,b, radius c (its right half)
};

float glyph_distance(int id, float u, float v) {
    static const std::vector<Stroke> kA = {{false, -0.55f, 0.75f, 0.0f, -0.75f}, {false, 0.0f, -0.75f, 0.55f, 0.75f},
                                           {false, -0.33f, 0.22f, 0.33f, 0.22f}};
    static const std::vector<Stroke> kB = {
        {false, -0.42f, -0.75f, -0.42f, 0.75f}, {false, -0.42f, -0.75f, 0.08f, -0.75f}, {true, 0.08f, -0.39f, 0.36f, 0},
        {false, 0.08f, -0.03f, -0.42f, -0.03f}, {false, -0.42f, -0.03f, 0.12f, -0.03f}, {true, 0.12f, 0.36f, 0.39f, 0},
        {false, 0.12f, 0.75f, -0.42f, 0.75f}};
    static const std::vector<Stroke> kX = {{false, -0.52f, -0.75f, 0.52f, 0.75f}, {false, 0.52f, -0.75f, -0.52f, 0.75f}};
    static const std::vector<Stroke> kY = {{false, -0.55f, -0.75f, 0.0f, 0.02f}, {false, 0.55f, -0.75f, 0.0f, 0.02f},
                                           {false, 0.0f, 0.02f, 0.0f, 0.75f}};
    static const std::vector<Stroke> kL = {{false, -0.38f, -0.75f, -0.38f, 0.75f}, {false, -0.38f, 0.75f, 0.45f, 0.75f}};
    static const std::vector<Stroke> kR = {
        {false, -0.42f, -0.75f, -0.42f, 0.75f}, {false, -0.42f, -0.75f, 0.08f, -0.75f}, {true, 0.08f, -0.375f, 0.375f, 0},
        {false, 0.08f, 0.0f, -0.42f, 0.0f}, {false, 0.0f, 0.0f, 0.5f, 0.75f}};
    static const std::vector<Stroke> kZ = {{false, -0.5f, -0.75f, 0.5f, -0.75f}, {false, 0.5f, -0.75f, -0.5f, 0.75f},
                                           {false, -0.5f, 0.75f, 0.5f, 0.75f}};
    static const std::vector<Stroke> kPlus = {{false, -0.62f, 0.0f, 0.62f, 0.0f}, {false, 0.0f, -0.62f, 0.0f, 0.62f}};
    static const std::vector<Stroke> kMinus = {{false, -0.62f, 0.0f, 0.62f, 0.0f}};
    auto dist = [](const std::vector<Stroke>& s, float x, float y) {
        float d = 1e9f;
        for (const Stroke& k : s)
            d = std::min(d, k.is_arc ? arc(x, y, k.a, k.b, k.c, -kPi / 2, kPi / 2) : seg(x, y, k.a, k.b, k.c, k.d));
        return d;
    };
    switch (id) {
        case kGlyphA: return dist(kA, u, v);
        case kGlyphB: return dist(kB, u, v);
        case kGlyphX: return dist(kX, u, v);
        case kGlyphY: return dist(kY, u, v);
        case kGlyphL: return dist(kL, u, v);
        case kGlyphR: return dist(kR, u, v);
        case kGlyphPlus: return dist(kPlus, u, v);
        case kGlyphMinus: return dist(kMinus, u, v);
        case kGlyphZL:
        case kGlyphZR: {  // two letters in a 2:1 cell (u in -2..2), each 0.8 of a single label
            const float s = 1.0f / 0.8f;
            const float z = dist(kZ, (u + 0.62f) * s, v * s) / s;
            const float l = dist(id == kGlyphZL ? kL : kR, (u - 0.62f) * s, v * s) / s;
            return std::min(z, l);
        }
        default: return 1e9f;
    }
}

void draw_glyph(Canvas& cv, int id) {
    cv.layer(kCells[id], [&](float u, float v, float pixel) {
        return grey(1.0f, line(glyph_distance(id, u, v), 0.125f, pixel));
    });
}

std::vector<uint8_t> draw_all() {
    Canvas cv;
    draw_range(cv, kStickBase, false);
    draw_range(cv, kStickBaseActive, true);
    draw_thumb(cv, kStickThumb, false);
    draw_thumb(cv, kStickThumbActive, true);
    auto circle = [](float u, float v) { return std::hypot(u, v) - 0.94f; };
    auto stadium = [](float u, float v) { return round_box(u, v, 1.92f, 0.92f, 0.92f, 0.92f, 0.92f, 0.92f); };
    // Triggers (cell u -1.67..1.67): the outer top corner rounded wide, like a Joy-Con's trigger seen from above.
    auto trigger_l = [](float u, float v) { return round_box(u, v, 1.60f, 0.90f, 0.86f, 0.30f, 0.30f, 0.30f); };
    auto trigger_r = [](float u, float v) { return round_box(u, v, 1.60f, 0.90f, 0.30f, 0.86f, 0.30f, 0.30f); };
    draw_body(cv, kDisc, false, circle);
    draw_body(cv, kDiscHeld, true, circle);
    draw_body(cv, kPill, false, stadium);
    draw_body(cv, kPillHeld, true, stadium);
    draw_body(cv, kTriggerL, false, trigger_l);
    draw_body(cv, kTriggerLHeld, true, trigger_l);
    draw_body(cv, kTriggerR, false, trigger_r);
    draw_body(cv, kTriggerRHeld, true, trigger_r);
    draw_dpad(cv);
    cv.layer(kCells[kRing], [](float u, float v, float pixel) {  // a soft white ring, scaled out and faded by the press
        const float r = std::hypot(u, v);
        const float core = line(r - 0.90f, 0.035f, pixel);
        const float glow = (1.0f - smooth(0.0f, 0.10f, std::fabs(r - 0.90f))) * 0.45f;
        return grey(1.0f, std::max(core * 0.9f, glow));
    });
    for (int id = kGlyphA; id <= kGlyphZR; id++) draw_glyph(cv, id);
    std::vector<uint8_t> out(cv.px.size());
    for (size_t i = 0; i < out.size(); i++) out[i] = uint8_t(clamp01(cv.px[i]) * 255.0f + 0.5f);
    return out;
}

std::once_flag g_once;
std::vector<uint8_t> g_pixels;
std::atomic<bool> g_ready{false};

}  // namespace

const Cell& cell(int id) { return kCells[std::clamp(id, 0, int(kCount) - 1)]; }

const uint8_t* atlas() {
    std::call_once(g_once, [] {
        g_pixels = draw_all();
        g_ready.store(true, std::memory_order_release);
    });
    return g_pixels.data();
}

bool atlas_ready() { return g_ready.load(std::memory_order_acquire); }

void prepare_atlas() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread([] { atlas(); }).detach(); });
}

}  // namespace nx::plat::art
