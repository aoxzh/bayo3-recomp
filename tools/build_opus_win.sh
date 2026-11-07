#!/usr/bin/env bash
# Build libopus for Windows x86-64, once, so the runtime can open it at run time.
#
# The runtime loads the host's own libopus at run time (runtime/src/svc_opus.cpp: LoadLibraryA "libopus.dll" next to
# the executable) and needs the codec only to decode the game's hwopus streams. Linux and macOS have a system package
# for it; Windows has no libopus in the platform at all, so the library has to be built and shipped next to the game
# executable (private/build-win/libopus.dll).
#
# Opus is BSD-licensed (COPYING in the tarball); the built library is redistributed under those terms.
#
# Usage: tools/build_opus_win.sh [--version 1.5.2] [--jobs N] [--force]
# Output: private/build-opus-win/libopus.dll   (gitignored: private/)
#
# The compiler must be on PATH (see docs/BUILD.md "Windows 10/11"): llvm-mingw's clang is used through
# <triplet>-clang, so the produced DLL links against the same C runtime as the game.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=1.5.2
jobs="$(nproc 2>/dev/null || echo 4)"
force=0
work="$root/private/opus-src"
out="$root/private/build-opus-win"
triplet="${B3_WIN_TRIPLET:-x86_64-w64-mingw32}"

usage() {
    printf '%s\n' \
        'Usage: tools/build_opus_win.sh [--version X.Y.Z] [--jobs N] [--force]' \
        'Builds libopus for Windows x86-64 into private/build-opus-win/libopus.dll, where the game finds it.' \
        'Environment: B3_WIN_TRIPLET (default x86_64-w64-mingw32).'
}
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

while (( $# )); do
    case "$1" in
        --version) version="${2:?--version needs a value}"; shift 2 ;;
        --jobs) jobs="${2:?--jobs needs a value}"; shift 2 ;;
        --force) force=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) fail "unknown argument: $1 (see --help)" ;;
    esac
done

for tool in cmake tar; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool missing: $tool"
done
command -v "$triplet-clang" >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 ||
    fail "no Windows C compiler on PATH: $triplet-clang (see docs/BUILD.md)"

library="$out/libopus.dll"
if [[ -f "$library" && "$force" = 0 ]]; then
    printf 'already built: %s\n' "$library"
    exit 0
fi

mkdir -p "$work" "$out"
tarball="$work/opus-$version.tar.gz"
if [[ ! -f "$tarball" ]]; then
    printf 'downloading opus %s\n' "$version"
    curl -fsSL -o "$tarball.part" "https://downloads.xiph.org/releases/opus/opus-$version.tar.gz"
    mv -- "$tarball.part" "$tarball"
fi
if [[ ! -d "$work/opus-$version" ]]; then
    tar xzf "$tarball" -C "$work"
fi

build="$work/opus-$version/build-win"
cmake -S "$work/opus-$version" -B "$build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="$triplet-clang" \
    -DCMAKE_RC_COMPILER="$triplet-windres" \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPUS_BUILD_SHARED_LIBRARY=ON -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_FIXED_POINT=OFF
cmake --build "$build" --parallel "$jobs"

[[ -f "$build/libopus.dll" ]] || fail "build produced no libopus.dll in $build"
cp -- "$build/libopus.dll" "$library"

# The same BSD notice the tarball carries, kept beside the binary for redistribution.
cp -- "$work/opus-$version/COPYING" "$out/COPYING" 2>/dev/null || true
printf 'libopus %s for Windows x86-64: %s (%s bytes)\n' \
    "$version" "$library" "$(stat -c %s "$library")"
