#!/usr/bin/env bash
# Linux / Steam Deck build. Defaults to a Debug build of the game with the unit tests enabled.
#
# Optional environment for checked builds (docs/SANITIZERS.md):
#   SANITIZE=undefined          # UBSan; CMake verifies the compiler-rt runtime links before anything is built
#   STL_DEBUG=1                 # libstdc++ debug containers (_GLIBCXX_DEBUG + PEDANTIC), pair with SANITIZE=undefined
#   NX_LLVM_ROOT=/opt/llvm      # LLVM with compiler-rt; its bin/ is put first on PATH (clang = this LLVM)
#   CC=clang CXX=clang++        # explicit compiler override (wins over PATH)
#
# When SANITIZE/STL_DEBUG are unset the configure command below is exactly the historical one (options default off).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export TMPDIR="$ROOT/private/tmp"
mkdir -p "$TMPDIR"
BUILD_DIR=${BUILD_DIR:-"$ROOT/private/build-linux"}
GEN_DIR=${GEN_DIR:-"$ROOT/private/bayonetta3/gen"}
VULKAN_HEADERS=${VULKAN_HEADERS:-"$ROOT/private/toolchains/Vulkan-Headers-1.4.357/include"}
SDL_PREFIX=${SDL_PREFIX:-}
JOBS=${JOBS:-2}
RECOMP_OPT=${RECOMP_OPT:-0}
SANITIZE=${SANITIZE:-}
STL_DEBUG=${STL_DEBUG:-0}
NX_LLVM_ROOT=${NX_LLVM_ROOT:-}

if [[ -n "$NX_LLVM_ROOT" ]]; then
    if [[ ! -x "$NX_LLVM_ROOT/bin/clang++" ]]; then
        printf 'NX_LLVM_ROOT=%s has no executable bin/clang++; point it at an LLVM install (docs/SANITIZERS.md).\n' \
            "$NX_LLVM_ROOT" >&2
        exit 1
    fi
    # The compiler-rt runtime must match the driver that links it, so use this LLVM's clang (unless CC/CXX say
    # otherwise). The CMake configure step then performs a real link probe and refuses to continue if it fails.
    export PATH="$NX_LLVM_ROOT/bin:$PATH"
fi

if [[ ! -f "$VULKAN_HEADERS/vulkan/vulkan.h" ]]; then
    printf 'Set VULKAN_HEADERS to Vulkan-Headers 1.4.357 or newer include directory.\n' >&2
    exit 1
fi
for mod in main subsdk0 sdk; do
    if [[ ! -f "$GEN_DIR/$mod/table.cpp" ]]; then
        printf 'Missing %s/table.cpp; run tools/prepare_bayonetta3.py --generate first.\n' "$GEN_DIR/$mod" >&2
        exit 1
    fi
done

extra_cmake_args=()
# Let CMake interpret CMAKE_PREFIX_PATH from the environment when SDL_PREFIX is unset.
if [[ -n "$SDL_PREFIX" ]]; then
    extra_cmake_args+=("-DCMAKE_PREFIX_PATH=$SDL_PREFIX")
fi
if [[ -n "$SANITIZE" ]]; then
    printf 'SANITIZE=%s: the configure step verifies the compiler-rt sanitizer runtime links (docs/SANITIZERS.md).\n' \
        "$SANITIZE"
    extra_cmake_args+=("-DNX_SANITIZE=$SANITIZE")
fi
if [[ "$STL_DEBUG" == "1" ]]; then
    extra_cmake_args+=("-DNX_STL_DEBUG=ON")
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DCMAKE_BUILD_TYPE=Debug -DNX_GAME=bayonetta3 -DNX_TESTS=ON \
    -DNX_GEN_DIR="$GEN_DIR" -DNX_RECOMP_OPT="$RECOMP_OPT" \
    -DNX_VULKAN_INCLUDE="$VULKAN_HEADERS" \
    ${extra_cmake_args[@]+"${extra_cmake_args[@]}"} "$@"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
