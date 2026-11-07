// Deliberate undefined behaviour for the sanitizer toolchain smoke check (scripts/ubsan_smoke.sh).
//
// This is NOT a unit test and it is never built by the default build: only the ubsan_smoke ctest invokes it, and that
// test skips itself unless the build was configured with -DNX_SANITIZE=undefined. Built with the project's exported
// sanitizer flags, UBSan must print "runtime error: signed integer overflow" and, with the default
// -fno-sanitize-recover=all, abort. Built without them, the program runs to completion (volatile keeps the addition
// from being folded away at -O2/-O3), which is what proves the report comes from UBSan and not from the program.
#include <cstdio>

int main(int argc, char**) {
    volatile int maximum = 2147483647;  // INT_MAX: adding anything positive overflows
    volatile int one = argc;            // argc == 1 in the normal case
    int overflowed = maximum + one;     // signed integer overflow: undefined behaviour
    std::printf("signed overflow computed as %d (expected a UBSan report above this line)\n", overflowed);
    return 0;
}
