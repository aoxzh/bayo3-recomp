// POSIX build of powzix/ooz: the few MSVC/Win32 names it uses, as GCC/Clang builtins. MIT, this project's own.
#pragma once
#include <stdint.h>
#include <string.h>
#include <time.h>

#define __forceinline inline __attribute__((always_inline))
#define __int64 long long
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline unsigned char _BitScanReverse(unsigned long* index, unsigned long mask) {
    if (!mask) return 0;
    *index = 31u - (unsigned long)__builtin_clz((unsigned)mask);
    return 1;
}
static inline unsigned char _BitScanForward(unsigned long* index, unsigned long mask) {
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctz((unsigned)mask);
    return 1;
}
static inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
// 32-bit like MSVC's unsigned long: a wider result would keep bits a later shift must drop.
static inline unsigned int _byteswap_ulong(unsigned int v) { return __builtin_bswap32(v); }
static inline unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }
#ifndef _rotl  // x86intrin.h (through intrin.h) already maps it to __rold
static inline unsigned int _rotl(unsigned int v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
#endif

typedef long long LARGE_INTEGER;
static inline int QueryPerformanceCounter(LARGE_INTEGER* t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *t = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}
static inline int QueryPerformanceFrequency(LARGE_INTEGER* f) { *f = 1000000000LL; return 1; }

// ooz's CLI can also load Oodle's own DLL to compare against it; the worker never calls that, so it finds nothing.
#define WINAPI
typedef void* HINSTANCE;
static inline HINSTANCE LoadLibraryA(const char*) { return 0; }
static inline void* GetProcAddress(HINSTANCE, const char*) { return 0; }
