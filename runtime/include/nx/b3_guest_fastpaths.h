// Game-specific fast paths for Bayonetta 3 1.2's generated guest functions.
#pragma once

namespace nx {
struct CPU;

namespace b3 {
// Return true only when the guest function has been handled; false leaves CPU
// state and guest memory unchanged so the generated body can run unchanged.
bool try_quad_walk(CPU& c);
bool try_morton_split(CPU& c);
}  // namespace b3
}  // namespace nx
