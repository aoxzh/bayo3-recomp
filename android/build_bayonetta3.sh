#!/usr/bin/env bash
# B3 build: libbayonetta3.so, and with --apk the launcher plus NativeActivity APK. No device actions or game data.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="$root/private/build-android"
build="$build_root/native"
out="$build_root/apk"
ndk="${ANDROID_NDK_ROOT:-}"
sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
gen="${NX_GEN_DIR:-$root/private/bayonetta3/gen}"
vulkan_headers="${VULKAN_HEADERS:-$root/private/toolchains/Vulkan-Headers-1.4.357/include}"
apk=0
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

usage() {
    printf '%s\n' \
        'Usage: android/build_bayonetta3.sh [--gen-dir DIR] [--jobs N] [--apk]' \
        'Default: configure NX_GAME=bayonetta3 and build libbayonetta3.so only.' \
        '  --gen-dir DIR  Generated sources containing main/, subsdk0/, sdk/.' \
        '  --jobs N       Parallel build jobs (default: 2).' \
        '  --apk          Also package and development-sign the APK (launcher + game).' \
        '  --help         Show this help.' \
        'Environment: NX_GEN_DIR, ANDROID_NDK_ROOT, ANDROID_SDK_ROOT (or ANDROID_HOME),' \
        '             CMAKE_BUILD_PARALLEL_LEVEL, VULKAN_HEADERS (include directory).' \
        'Default Vulkan headers: private/toolchains/Vulkan-Headers-1.4.357/include.' \
        'APK overrides: ANDROID_BUILD_TOOLS_VERSION, ANDROID_COMPILE_SDK (number >=35).' \
        'Build outputs/key: private/build-android/; TMPDIR is forced to private/tmp/.'
}
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "Required tool not found: $1"; }
while (( $# )); do
    case "$1" in
        --gen-dir|--jobs)
            (( $# >= 2 )) && [[ -n "$2" && "$2" != --* ]] || fail "$1 needs a value"
            if [[ "$1" == --gen-dir ]]; then gen="$2"; else jobs="$2"; fi
            shift 2 ;;
        --apk) apk=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) fail "Unknown argument: $1 (see --help)" ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail '--jobs / CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer'
need cmake
need ninja
[[ -n "$ndk" && -f "$ndk/build/cmake/android.toolchain.cmake" ]] || fail "NDK toolchain missing: ${ndk:-<unset>}; set ANDROID_NDK_ROOT"
ndk="$(cd -- "$ndk" && pwd)"
[[ -d "$gen" ]] || fail "Generated code directory missing: $gen; use --gen-dir or NX_GEN_DIR"
gen="$(cd -- "$gen" && pwd)"
shopt -s nullglob
for module in main subsdk0 sdk; do
    sources=("$gen/$module/"*.cpp)
    (( ${#sources[@]} )) || fail "No generated C++ sources in $gen/$module"
done
[[ -f "$vulkan_headers/vulkan/vulkan.h" && -f "$vulkan_headers/vulkan/vulkan_core.h" ]] || \
    fail "Vulkan headers missing: $vulkan_headers; set VULKAN_HEADERS to the Vulkan-Headers 1.4.357 include directory"
vulkan_headers="$(cd -- "$vulkan_headers" && pwd)"
# The shared /tmp has little space. All child build/package tools inherit this path.
export TMPDIR="$root/private/tmp"
mkdir -p "$TMPDIR"
[[ -w "$TMPDIR" ]] || fail "Temporary directory is not writable: $TMPDIR"

# APK tools are optional for native builds. Check them before the expensive build.
if (( apk )); then
    [[ -n "$sdk" && -d "$sdk" ]] || fail 'Android SDK missing; set ANDROID_SDK_ROOT or ANDROID_HOME'
    need java
    need javac
    need keytool
    need zip
    need sort
    bt=''
    if [[ -n "${ANDROID_BUILD_TOOLS_VERSION:-}" ]]; then
        bt="$sdk/build-tools/$ANDROID_BUILD_TOOLS_VERSION"
    else
        versions=()
        for candidate in "$sdk"/build-tools/*; do
            version="${candidate##*/}"
            [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || continue
            [[ -x "$candidate/aapt2" && -x "$candidate/zipalign" && -x "$candidate/apksigner" && -x "$candidate/d8" ]] || continue
            versions+=("$version")
        done
        (( ${#versions[@]} )) || fail "No complete SDK build-tools in $sdk; omit --apk for a native-only build"
        mapfile -t versions < <(printf '%s\n' "${versions[@]}" | sort -V)
        bt="$sdk/build-tools/${versions[${#versions[@]}-1]}"
    fi
    for tool in aapt2 zipalign apksigner d8; do
        [[ -x "$bt/$tool" ]] || fail "APK tool missing: $bt/$tool"
    done
    bt="$(cd -- "$bt" && pwd)"
    compile_sdk="${ANDROID_COMPILE_SDK:-}"
    if [[ -z "$compile_sdk" ]]; then
        compile_sdk=0
        for candidate in "$sdk"/platforms/android-*; do
            version="${candidate##*/android-}"
            [[ "$version" =~ ^[1-9][0-9]*$ && -f "$candidate/android.jar" ]] || continue
            if (( version > compile_sdk )); then compile_sdk="$version"; fi
        done
    fi
    [[ "$compile_sdk" =~ ^[1-9][0-9]*$ ]] || fail 'ANDROID_COMPILE_SDK must be a numeric API level'
    (( compile_sdk >= 35 )) || fail 'APK packaging needs an installed Android platform >=35 (targetSdk=35)'
    jar="$sdk/platforms/android-$compile_sdk/android.jar"
    [[ -f "$jar" ]] || fail "Android platform missing: $jar"
    case "$(uname -s)" in
        Linux) host=linux-x86_64 ;;
        Darwin) host=darwin-x86_64 ;;
        *) fail 'APK stripping currently supports Linux and macOS NDK hosts' ;;
    esac
    strip="$ndk/toolchains/llvm/prebuilt/$host/bin/llvm-strip"
    [[ -x "$strip" ]] || fail "NDK strip tool missing: $strip"
    "$bt/aapt2" version >/dev/null
    "$bt/apksigner" version >/dev/null
fi

# Reconfigure every invocation so NX_GEN_DIR and the selected NDK cannot go stale.
# NDK r28b's Vulkan headers (275) are too old for the generated Vulkan interfaces.
# Project CMake places NX_VULKAN_INCLUDE before the NDK sysroot headers.
printf 'B3 native build: API 29, arm64-v8a, armv8.2-a+crypto\nNDK: %s\nGenerated sources: %s\nVulkan headers: %s\nTMPDIR: %s\n' \
    "$ndk" "$gen" "$vulkan_headers" "$TMPDIR"
cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_C_FLAGS=-march=armv8.2-a+crypto \
    -DCMAKE_CXX_FLAGS=-march=armv8.2-a+crypto \
    -DNX_GAME=bayonetta3 -DNX_GEN_DIR="$gen" -DNX_VULKAN_INCLUDE="$vulkan_headers"
cmake --build "$build" --target bayonetta3 --parallel "$jobs"
native="$build/libbayonetta3.so"
[[ -f "$native" ]] || fail "Expected native library missing: $native"
printf 'Native library: %s\n' "$native"
(( apk )) || exit 0

mkdir -p "$out"
stage="$(mktemp -d "$out/.stage.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
mkdir -p "$stage/lib/arm64-v8a"
"$strip" --strip-unneeded -o "$stage/lib/arm64-v8a/libbayonetta3.so" "$native"
# The game's hwopus service decodes with the host's libopus, which Android does not have in the platform at all, so a
# built copy rides in the APK (tools/build_opus_android.sh). The runtime finds it with a plain dlopen("libopus.so"):
# extractNativeLibs unpacks the app's lib folder at install time and the loader searches it first. Without the file the
# packaging still succeeds and the game stops at startup with "hwopus: host libopus unavailable" — say which file.
opus="$root/private/toolchains/opus-android/arm64-v8a/libopus.so"
apk_libs=(lib/arm64-v8a/libbayonetta3.so)
if [[ -f "$opus" ]]; then
    "$strip" --strip-unneeded -o "$stage/lib/arm64-v8a/libopus.so" "$opus"
    apk_libs+=(lib/arm64-v8a/libopus.so)
    printf 'libopus: %s\n' "$opus"
else
    printf 'warning: %s missing; the APK will stop at startup in hwopus.\n' "$opus" >&2
    printf 'warning: build it first with tools/build_opus_android.sh\n' >&2
fi
# The launcher (games/bayonetta3/java): its art as an asset, its Java through javac and d8 (no Gradle). No importer,
# RomFS, NSO, keys or shaders are added.
mkdir -p "$stage/assets/launcher" "$stage/classes" "$stage/dex"
cp "$root/tools/launcher_assets/background.jpg" "$stage/assets/launcher/background.jpg"
"$bt/aapt2" link -o "$stage/base.apk" -I "$jar" \
    --manifest "$root/games/bayonetta3/AndroidManifest.xml" \
    --min-sdk-version 29 --target-sdk-version 35 -A "$stage/assets"
# --release 8 with android.jar on the classpath: API 36's android.jar has no LambdaMetafactory javac 21+ accepts.
mapfile -t java_sources < <(find "$root/games/bayonetta3/java" -name '*.java' | sort)
javac --release 8 -Xlint:-options -encoding UTF-8 -classpath "$jar" -d "$stage/classes" "${java_sources[@]}"
mapfile -t class_files < <(find "$stage/classes" -name '*.class' | sort)
"$bt/d8" --release --min-api 29 --lib "$jar" --output "$stage/dex" "${class_files[@]}"
cp "$stage/dex/classes.dex" "$stage/classes.dex"
(cd -- "$stage" && zip -q -9 base.apk "${apk_libs[@]}" classes.dex)
# Native libraries are compressed and extracted (manifest extractNativeLibs=true).
"$bt/zipalign" -f 4 "$stage/base.apk" "$stage/aligned.apk"
keydir="$build_root/signing"
keystore="$keydir/development.keystore"
if [[ ! -f "$keystore" ]]; then
    (
        umask 077
        mkdir -p "$keydir"
        keytool -genkeypair -noprompt -keystore "$keystore" -storetype JKS \
            -alias androiddebugkey -storepass android -keypass android \
            -keyalg RSA -keysize 2048 -validity 10000 \
            -dname 'CN=B3 Development,OU=Local,O=Development,C=US'
    )
fi
chmod 700 "$keydir"
chmod 600 "$keystore"
"$bt/apksigner" sign --ks "$keystore" --ks-key-alias androiddebugkey \
    --ks-pass pass:android --key-pass pass:android \
    --out "$stage/bayonetta3.apk" "$stage/aligned.apk"
"$bt/apksigner" verify "$stage/bayonetta3.apk"
"$bt/zipalign" -c 4 "$stage/bayonetta3.apk"
mv -f -- "$stage/bayonetta3.apk" "$out/bayonetta3.apk"
printf 'Development APK: %s\n' "$out/bayonetta3.apk"
