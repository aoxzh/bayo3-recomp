#!/usr/bin/env bash
# Build libopus for Android arm64-v8a, once, so android/build_bayonetta3.sh can put it in the APK.
#
# The runtime loads the host's own libopus at run time (runtime/src/svc_opus.cpp: dlopen "libopus.so.0" then
# "libopus.so") and needs the codec only to decode the game's hwopus streams. Linux and macOS have a system package
# for it; Android has no libopus in the platform at all (`/system/lib64/libopus*` does not exist), so the library has
# to be built and shipped inside the APK, next to libbayonetta3.so. extractNativeLibs is true in the manifest, so it is
# unpacked into the app's lib folder at install time and dlopen("libopus.so") finds it.
#
# Opus is BSD-licensed (COPYING in the tarball); the built library is redistributed under those terms.
#
# Usage: tools/build_opus_android.sh [--version 1.5.2] [--jobs N] [--force]
# Output: private/toolchains/opus-android/arm64-v8a/libopus.so   (gitignored: private/)
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=1.5.2
jobs="$(nproc 2>/dev/null || echo 4)"
force=0
ndk="${ANDROID_NDK_ROOT:-}"
work="$root/private/opus-src"
out="$root/private/toolchains/opus-android/arm64-v8a"
libraries="$root/private/toolchains/opus-android"

usage() {
    printf '%s\n' \
        'Usage: tools/build_opus_android.sh [--version X.Y.Z] [--jobs N] [--force]' \
        'Builds libopus for Android arm64-v8a into private/toolchains/opus-android/arm64-v8a/libopus.so,' \
        'where android/build_bayonetta3.sh picks it up for the APK.' \
        'Environment: ANDROID_NDK_ROOT (required; path to the Android NDK).'
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

[[ -n "$ndk" && -f "$ndk/build/cmake/android.toolchain.cmake" ]] || fail "NDK toolchain missing: ${ndk:-<unset>}; set ANDROID_NDK_ROOT"
for tool in cmake ninja curl tar; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool missing: $tool"
done

library="$out/libopus.so"
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
rm -rf -- "$work/opus-$version"
tar xzf "$tarball" -C "$work"

build="$work/opus-$version/build-android"
cmake -S "$work/opus-$version" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS=-march=armv8.2-a+crypto \
    -DOPUS_BUILD_SHARED_LIBRARY=ON -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_FLOAT_APPROX=OFF
cmake --build "$build" --parallel "$jobs"

[[ -f "$build/libopus.so" ]] || fail "build produced no libopus.so in $build"
cp -- "$build/libopus.so" "$library"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$library"

# The same BSD notice the tarball carries, kept beside the binary for redistribution.
cp -- "$work/opus-$version/COPYING" "$libraries/COPYING" 2>/dev/null || true
printf 'libopus %s for Android arm64-v8a: %s (%s bytes)\n' \
    "$version" "$library" "$(stat -c %s "$library")"
