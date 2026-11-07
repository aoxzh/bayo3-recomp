#!/usr/bin/env bash
# Preflight: the checks that must be green before anyone says "done" on this repository.
#
# WHY THIS FILE EXISTS
#   The three ways a cross-platform session was wasted (docs/CROSS-PLATFORM.md) are all detectable without a
#   GPU: `bash -n` catches a script edited into a syntax error and never re-run; the contract test catches the
#   two entry points drifting apart or hand-building driver flags; acceptance_status.py shows, without
#   judgment, which platform last verified which commit. Running this before claiming a result is cheaper
#   than discovering the difference on the other host.
#
#   Never starts a game, never builds one. The harness tests that touch the entry points use B3_DRY_RUN=1.
#
# usage: scripts/b3_preflight.sh              # fast checks (no build, no ctest, no game)
#        scripts/b3_preflight.sh --ctest      # also run ctest in an existing build tree (slow)
#        B3_BUILD_DIR=/path/to/build scripts/b3_preflight.sh --ctest
#
# Exit code: 0 = every check passed (a SKIP is not a failure), 1 = at least one check failed.
set -u
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
FAILED=0
SKIPPED=0

usage() {
    cat <<'EOF'
usage: scripts/b3_preflight.sh [--ctest]

  (no option)  bash -n every shell script, compile tools/ and tests/ python, run the cross-platform
               contract test, print the acceptance status table. No build, no ctest, no game.
  --ctest      additionally run ctest in the existing build tree (B3_BUILD_DIR, default private/build-deck).
  -h, --help   this text.

Exit code 0 when every check passed; 1 when any failed. SKIP does not fail the run.
EOF
}

step() { printf '\n== %s\n' "$*"; }
ok() { printf 'PASS: %s\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*"; FAILED=1; }
skip() { printf 'SKIP: %s\n' "$*"; SKIPPED=$((SKIPPED + 1)); }

RUN_CTEST=0
for arg in "$@"; do
    case "$arg" in
        --ctest) RUN_CTEST=1 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'error: unknown argument: %s\n\n' "$arg" >&2; usage >&2; exit 2 ;;
    esac
done

# `command -v python3` is not enough on Windows: the Microsoft Store puts a python3.exe *stub* in PATH
# that prints "Python was not found" and exits 49 instead of running anything. Probe by running it
# (scripts/lib/b3_common.sh::b3_python is the same rule the entry points use).
. "$ROOT/scripts/lib/b3_common.sh"
B3_ROOT=$ROOT
if ! PY=$(b3_python) || ! "$PY" -c 'import sys' >/dev/null 2>&1; then
    printf 'error: a working python3 is required by the harness checks (set B3_PYTHON=/path/to/python)\n' >&2
    exit 1
fi

# Bytecode goes to a temporary cache, never into the repository (__pycache__ is gitignored, but a preflight
# run should still not write into a working tree it is inspecting).
PYCACHE=$(mktemp -d "${TMPDIR:-/tmp}/b3-preflight.XXXXXX") || exit 1
trap 'rm -rf "$PYCACHE"' EXIT
PYTHONPYCACHEPREFIX=$PYCACHE
export PYTHONPYCACHEPREFIX

# --- 1. every shell script parses ----------------------------------------------------------------
step "bash -n: scripts/*.sh and scripts/lib/*.sh"
PARSED=0
PARSE_FAILED=0
for script in "$ROOT"/scripts/*.sh "$ROOT"/scripts/lib/*.sh; do
    [ -e "$script" ] || continue
    PARSED=$((PARSED + 1))
    if ! output=$(bash -n "$script" 2>&1); then
        bad "${script#$ROOT/}: $output"
        PARSE_FAILED=1
    fi
done
if [ "$PARSE_FAILED" -eq 0 ]; then
    ok "$PARSED shell scripts parse"
fi

# --- 2. every python file in tools/ and tests/ compiles -------------------------------------------
step "python syntax: tools/*.py and tests/*.py"
COMPILED=0
COMPILE_FAILED=0
for py in "$ROOT"/tools/*.py "$ROOT"/tests/*.py; do
    [ -e "$py" ] || continue
    COMPILED=$((COMPILED + 1))
    if ! output=$("$PY" -m py_compile "$py" 2>&1); then
        bad "${py#$ROOT/}: $output"
        COMPILE_FAILED=1
    fi
done
if [ "$COMPILE_FAILED" -eq 0 ]; then
    ok "$COMPILED python files compile"
fi

# --- 3. the cross-platform contract test ---------------------------------------------------------
step "contract test: tests/test_cross_platform_harness.py"
HARNESS="$ROOT/tests/test_cross_platform_harness.py"
if [ ! -f "$HARNESS" ]; then
    skip "tests/test_cross_platform_harness.py is missing"
elif output=$(cd "$ROOT" && "$PY" "$HARNESS" 2>&1); then
    printf '%s\n' "$output"
    ok "cross-platform contract test"
else
    printf '%s\n' "$output"
    bad "cross-platform contract test"
fi

# --- 4. acceptance status (informational; --check is the gate) -------------------------------------
step "acceptance status: tools/acceptance_status.py"
STATUS_TOOL="$ROOT/tools/acceptance_status.py"
if [ ! -f "$STATUS_TOOL" ]; then
    skip "tools/acceptance_status.py is missing"
elif output=$(cd "$ROOT" && "$PY" "$STATUS_TOOL" 2>&1); then
    printf '%s\n' "$output"
    ok "acceptance status table"
else
    printf '%s\n' "$output"
    bad "acceptance status table"
fi
printf 'note: the CI gate is `python3 tools/acceptance_status.py --check`; it exits 1 until every platform\n'
printf '      has a passing record at HEAD (this is real information, not a broken tool).\n'

# --- 5. optional ctest ----------------------------------------------------------------------------
if [ "$RUN_CTEST" -eq 1 ]; then
    step "ctest (slow: it builds and runs the whole suite)"
    BUILD_DIR=${B3_BUILD_DIR:-$ROOT/private/build-deck}
    if [ ! -d "$BUILD_DIR" ]; then
        skip "no build tree at ${BUILD_DIR#$ROOT/} (set B3_BUILD_DIR); --ctest needs a configured build"
    elif output=$(cd "$BUILD_DIR" && ctest --output-on-failure 2>&1); then
        printf '%s\n' "$output" | tail -n 5
        ok "ctest"
    else
        printf '%s\n' "$output" | tail -n 60
        bad "ctest"
    fi
fi

# --- result --------------------------------------------------------------------------------------
printf '\n'
if [ "$FAILED" -ne 0 ]; then
    printf 'preflight: FAILED\n' >&2
    exit 1
fi
if [ "$SKIPPED" -gt 0 ]; then
    printf 'preflight: OK (%d skipped)\n' "$SKIPPED"
else
    printf 'preflight: OK\n'
fi
exit 0
