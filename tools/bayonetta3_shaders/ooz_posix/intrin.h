// POSIX build of powzix/ooz: MSVC's <intrin.h>. x86-64 gets the real SSE intrinsics; ARM64 (Android) gets them through
// sse2neon (fetched at its pinned commit beside the ooz source and put on the include path by the setup).
#pragma once
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#else
#include "sse2neon.h"
#endif
