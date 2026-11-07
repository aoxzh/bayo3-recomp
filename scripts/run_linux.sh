#!/usr/bin/env bash
# Keep mutable runtime files outside the preparation tool's atomic publication root.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
GENERATED="$ROOT/private/bayonetta3"
RUN_ROOT="$ROOT/private/run-bayonetta3"
BUILD_DIR=${BUILD_DIR:-"$ROOT/private/build-linux"}
SPIRV_DIR=${B3_SPIRV_DIR:-"$ROOT/private/bayonetta3-shaders-tess-hmflower-v1/spirv"}
BINARY="$BUILD_DIR/bayonetta3"

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf '%s\n' \
        'Usage: scripts/run_linux.sh [runtime arguments...]' \
        'Runs BUILD_DIR/bayonetta3 (default: private/build-linux/bayonetta3).' \
        'Mutable data: private/run-bayonetta3; NX_DATA_ROOT is set to this directory.' \
        'Links analysis/extract from private/bayonetta3 when their sources exist.' \
        'Shaders: B3_SPIRV_DIR (absolute; default: private/bayonetta3-shaders-tess-hmflower-v1/spirv).' \
        'Existing files or links to a different target are never replaced.'
    exit 0
fi
[[ "$SPIRV_DIR" == /* ]] || fail "B3_SPIRV_DIR must be an absolute path: $SPIRV_DIR"
[[ -x "$BINARY" && ! -d "$BINARY" ]] || fail "Executable missing: $BINARY; run scripts/build_linux.sh first"
[[ ! -L "$RUN_ROOT" ]] || fail "Runtime root must not be a symlink: $RUN_ROOT"
[[ ! -e "$RUN_ROOT" || -d "$RUN_ROOT" ]] || fail "Runtime root is not a directory: $RUN_ROOT"

# Check every destination before creating any links; reject even dangling foreign links.
for name in analysis extract spirv; do
    source="$GENERATED/$name"
    [[ "$name" != spirv ]] || source="$SPIRV_DIR"
    dest="$RUN_ROOT/$name"
    if [[ -L "$dest" ]]; then
        [[ $(readlink -- "$dest") == "$source" ]] || fail "Refusing foreign symlink: $dest"
    elif [[ -e "$dest" ]]; then
        fail "Refusing existing runtime data at link destination: $dest"
    fi
    [[ ! -e "$source" || -d "$source" ]] || fail "Generated source is not a directory: $source"
done
mkdir -p -- "$RUN_ROOT" "$ROOT/private/tmp"
for name in analysis extract spirv; do
    source="$GENERATED/$name"
    [[ "$name" != spirv ]] || source="$SPIRV_DIR"
    dest="$RUN_ROOT/$name"
    if [[ -d "$source" && ! -L "$dest" ]]; then
        # -T prevents ln from treating an unexpected destination directory as a container.
        ln -sT -- "$source" "$dest"
    fi
done
export NX_DATA_ROOT="$RUN_ROOT"
export TMPDIR="$ROOT/private/tmp"
printf 'B3 runtime data: %s\n' "$NX_DATA_ROOT"

# The console replaces the process when the game calls IApplicationFunctions::ExecuteProgram: the bar hub's
# "Records of Time" book does it with ProgramSpecifyKind 0 (ProgramIndex) and value 1, i.e. it starts
# 01004a4010fea001 - Bayonetta 3's second program, built separately from games/bayonetta3prog1 - and hands it
# the user-channel storage the old program pushed (LaunchParameterKind 1). The runtime ends with exit code 86
# and relaunch.txt ("<kind> <value>"), so this loop starts the requested program with the right data root and
# feeds it that storage as launch_parameter.txt. Bounded, so a game that keeps asking cannot loop forever.
PROG1_BINARY=${PROG1_BINARY:-$ROOT/private/build-prog1/bayonetta3prog1}
PROG1_DATA_ROOT=${PROG1_DATA_ROOT:-$ROOT/private/run-prog1}

program=0
root_for() { if [[ $1 == 0 ]]; then printf '%s' "$RUN_ROOT"; else printf '%s' "$PROG1_DATA_ROOT"; fi; }
exe_for() { if [[ $1 == 0 ]]; then printf '%s' "$BINARY"; else printf '%s' "$PROG1_BINARY"; fi; }
prepare_prog1_root() {
    # Its own analysis/extract (from the program-1 generation); user/ (the save data) shared with the main
    # game, exactly as both programs of one application share their save on the console.
    mkdir -p "$PROG1_DATA_ROOT"
    [[ -e $PROG1_DATA_ROOT/analysis ]] || ln -sT "$ROOT/private/prog1-out/analysis" "$PROG1_DATA_ROOT/analysis"
    [[ -e $PROG1_DATA_ROOT/extract ]] || ln -sT "$ROOT/private/prog1-out/extract" "$PROG1_DATA_ROOT/extract"
    [[ -e $PROG1_DATA_ROOT/user ]] || ln -sT "$RUN_ROOT/user" "$PROG1_DATA_ROOT/user"
}

attempts=0
while :; do
    exe=$(exe_for "$program")
    root=$(root_for "$program")
    [[ $program == 1 ]] && prepare_prog1_root
    if [[ ! -x $exe ]]; then
        printf 'B3: program %d binary missing: %s\n' "$program" "$exe" >&2
        exit 1
    fi
    # Capture nonzero status explicitly: set -e must not swallow the handoff code.
    rc=0
    NX_DATA_ROOT="$root" "$exe" "$@" || rc=$?
    if [[ $rc == 86 && $attempts -lt 8 ]]; then
        attempts=$((attempts + 1))
        kind=0; value=0
        [[ -f $root/relaunch.txt ]] && read -r kind value <"$root/relaunch.txt"
        next=0
        [[ $kind == 0 && $value == 1 ]] && next=1
        nextroot=$(root_for "$next")
        [[ $next == 1 ]] && prepare_prog1_root
        # What the old program pushed to the user channel becomes the new program's launch parameter.
        if [[ -f $root/user_channel.bin ]]; then
            cp -f "$root/user_channel.bin" "$nextroot/launch_parameter.txt"
        fi
        rm -f "$root/relaunch.txt"
        printf 'B3: program %d asked for program %d (kind %s, value %s); starting it (%d)\n' \
            "$program" "$next" "$kind" "$value" "$attempts"
        program=$next
        continue
    fi
    exit $rc
done
