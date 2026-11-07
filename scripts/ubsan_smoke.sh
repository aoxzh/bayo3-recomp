#!/usr/bin/env bash
# UBSan smoke check: prove that the sanitizer flags configured into this build actually link a runtime that reports
# undefined behaviour, by compiling a deliberately-UB program (tests/ubsan/ubsan_ub_smoke.cpp) with exactly those
# flags, running it, and asserting the UBSan diagnostic.
#
# Usage:
#   scripts/ubsan_smoke.sh                     # standalone: -fsanitize=undefined defaults, current $CXX
#   scripts/ubsan_smoke.sh --build-dir DIR     # use the flags exported by a configured CMake build
#   scripts/ubsan_smoke.sh --compiler clang++ --keep
#
# Exit codes: 0 = PASS (UBSan reported the deliberate UB), 1 = FAIL, 77 = SKIP (the build exports no UBSan flags).
# The build directory metadata (nx-sanitizer-flags.txt) is written by cmake/NXSanitizers.cmake. Registered as the
# ubsan_smoke ctest when NX_TESTS=ON; see docs/SANITIZERS.md.
set -u

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "${script_dir}/.." && pwd)

build_dir=""
compiler="${CXX:-}"
source_file="${repo_root}/tests/ubsan/ubsan_ub_smoke.cpp"
out_dir=""
keep=0

usage() {
    sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

skip() {
    printf 'SKIP: %s\n' "$*"
    exit 77
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) build_dir="${2:-}"; shift 2 || shift ;;
        --compiler)  compiler="${2:-}";  shift 2 || shift ;;
        --source)    source_file="${2:-}"; shift 2 || shift ;;
        --out)       out_dir="${2:-}";   shift 2 || shift ;;
        --keep)      keep=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) printf 'unknown argument: %s (try --help)\n' "$1" >&2; exit 2 ;;
    esac
done

# ---- where do the flags come from? -----------------------------------------------------------------------------

sanitizers="undefined"
recover="OFF"
flags=()

if [[ -n "${build_dir}" ]]; then
    meta_file="${build_dir}/nx-sanitizer-flags.txt"
    if [[ ! -f "${meta_file}" ]]; then
        skip "no ${meta_file}: this build was configured without NX_SANITIZE (or predates the sanitizer support)"
    fi
    while IFS= read -r line || [[ -n "${line}" ]]; do
        case "${line}" in
            COMPILER=*)   compiler="${line#COMPILER=}" ;;
            RECOVER=*)    recover="${line#RECOVER=}" ;;
            SANITIZERS=*) sanitizers="${line#SANITIZERS=}" ;;
            FLAG=*)       flags+=("${line#FLAG=}") ;;
        esac
    done < "${meta_file}"
    case "${sanitizers}" in
        *undefined*) ;;
        *) skip "this build uses NX_SANITIZE=${sanitizers}; the UBSan smoke check does not apply" ;;
    esac
    if [[ ${#flags[@]} -eq 0 ]]; then
        printf 'FAIL: %s exported no FLAG= lines\n' "${meta_file}"
        exit 1
    fi
else
    flags=(-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
fi

if [[ -z "${compiler}" ]]; then
    for candidate in clang++ c++; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            compiler=$(command -v "${candidate}")
            break
        fi
    done
fi
if [[ -z "${compiler}" ]]; then
    printf 'FAIL: no C++ compiler found; set CXX=/path/to/clang++\n'
    exit 1
fi
if [[ ! -f "${source_file}" ]]; then
    printf 'FAIL: source not found: %s\n' "${source_file}"
    exit 1
fi

cleanup_dir=0
if [[ -z "${out_dir}" ]]; then
    out_dir=$(mktemp -d "${TMPDIR:-/tmp}/nx-ubsan-smoke.XXXXXX") || exit 1
    cleanup_dir=1
else
    mkdir -p "${out_dir}" || exit 1
fi

cleanup() {
    if [[ ${cleanup_dir} -eq 1 && ${keep} -eq 0 ]]; then
        rm -rf "${out_dir}"
    fi
}
trap cleanup EXIT

printf '== UBSan smoke check ==\n'
printf 'compiler: %s\n' "$("${compiler}" --version 2>/dev/null | head -n 1)"
printf 'flags:    %s\n' "${flags[*]}"
printf 'source:   %s\n' "${source_file}"
printf 'workdir:  %s\n' "${out_dir}"

# ---- 1. the program itself is fine: with no sanitizer flags it exits 0 -----------------------------------------

plain_bin="${out_dir}/ubsan_smoke_plain"
if ! "${compiler}" -std=c++20 "${source_file}" -o "${plain_bin}" >"${out_dir}/plain-build.log" 2>&1; then
    printf 'FAIL: the smoke program does not compile without sanitizer flags:\n'
    sed -n '1,20p' "${out_dir}/plain-build.log"
    exit 1
fi
"${plain_bin}" >"${out_dir}/plain.log" 2>&1
plain_rc=$?
if [[ ${plain_rc} -ne 0 ]]; then
    printf 'FAIL: the deliberate-UB program exits %d without sanitizer flags; the program is broken, not the toolchain.\n' "${plain_rc}"
    sed -n '1,10p' "${out_dir}/plain.log"
    exit 1
fi
printf 'plain (no sanitizer):       PASS (exits 0, expected)\n'

# ---- 2. the same program with the build's exported flags: UBSan must report ----------------------------------
# The exported compile/link flags are used verbatim; -g and -fno-omit-frame-pointer come with them.

san_bin="${out_dir}/ubsan_smoke_ubsan"
if ! "${compiler}" -std=c++20 "${flags[@]}" "${source_file}" -o "${san_bin}" >"${out_dir}/ubsan-build.log" 2>&1; then
    printf 'FAIL: the smoke program does not link/compile with the exported sanitizer flags:\n'
    sed -n '1,20p' "${out_dir}/ubsan-build.log"
    exit 1
fi

# The outer 2>/dev/null throws away bash's own "Abort trap" job notice for the expected abort; the program's stderr
# (the UBSan report) is captured by the inner redirection.
{ UBSAN_OPTIONS="${SMOKE_UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" "${san_bin}" >"${out_dir}/ubsan.log" 2>&1; } 2>/dev/null
san_rc=$?

reported=0
grep -q 'runtime error' "${out_dir}/ubsan.log" && reported=1
overflow=0
grep -q 'signed integer overflow' "${out_dir}/ubsan.log" && overflow=1

printf '%s\n' '--- UBSan output (first 15 lines) ---'
sed -n '1,15p' "${out_dir}/ubsan.log" | sed 's/^/  /'
printf '%s\n' '------------------------------------'

status=0
if [[ ${reported} -eq 1 && ${overflow} -eq 1 ]]; then
    printf 'UBSan report:              PASS ("signed integer overflow" reported)\n'
else
    printf 'UBSan report:              FAIL (no "runtime error: signed integer overflow" in the output)\n'
    status=1
fi

if [[ "${recover}" != "ON" ]]; then
    if [[ ${san_rc} -ne 0 ]]; then
        printf 'halt on first report:      PASS (exit %d under -fno-sanitize-recover=all)\n' "${san_rc}"
    else
        printf 'halt on first report:      FAIL (program exited 0 despite -fno-sanitize-recover=all)\n'
        status=1
    fi
else
    printf 'halt on first report:      n/a (build uses NX_SANITIZE_RECOVER=ON, recovery allowed)\n'
fi

# ---- 3. macOS: the binary must link the UBSan runtime dylib --------------------------------------------------

if [[ "$(uname -s)" == "Darwin" ]] && command -v otool >/dev/null 2>&1; then
    otool_line=$(otool -L "${san_bin}" 2>/dev/null | grep -m 1 'libclang_rt\.ubsan' || true)
    if [[ -n "${otool_line}" ]]; then
        printf 'otool -L:                  PASS (%s)\n' "$(printf '%s' "${otool_line}" | sed 's/^[[:space:]]*//')"
    else
        printf 'otool -L:                  FAIL (no libclang_rt.ubsan* in the sanitized binary)\n'
        status=1
    fi
fi

if [[ ${status} -eq 0 ]]; then
    printf 'result: PASS\n'
    if [[ ${cleanup_dir} -eq 0 ]]; then
        printf 'artifacts: %s\n' "${out_dir}"
    fi
    exit 0
fi

keep=1
cleanup_dir=0
printf 'result: FAIL (artifacts kept in %s)\n' "${out_dir}"
exit 1
