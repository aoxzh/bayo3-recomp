#!/usr/bin/env bash
# One unattended headless Bayonetta 3 run on the macOS host.
#
#   scripts/b3_mac_run.sh <name> <seconds> [KEY=VALUE ...]
#
# Same contract as the Deck's scripts/b3_experiment.sh (scripts/lib/b3_common.sh): one definition of
# the run environment, of the machine lock and of the driver arguments, so a flag cannot be passed
# on one host and silently missing on the other. Only the host specifics differ here (build/data
# paths, no flock, a data root that has to be bootstrapped from the read-only inputs).
#
# B3_DRY_RUN=1 prints the assembled game/driver commands and exits without launching anything.
#
# Environment:
#   EXE         game binary
#               (default <repo>/private/build-mac/bayonetta3)
#   DATA_ROOT   NX_DATA_ROOT (default <repo>/private/B3-data/run)
#   LOGS        log dir     (default <repo>/logs/mac)
#   AUTODRIVE   1: run the shared autodrive for the same <seconds> (default 0: just run)
#   EVERY       autodrive poll seconds (default 1.5)
#   TAKE_YES    1: the driver takes the YES option on YES/No pages (default 0; the completed-save
#               hub's chapter-start confirm needs it, an intro/Prologue YES/No wants the No)
#   FRAMES      1: set NX_DUMP_EVERY_S=0.5 for the whole run (default 0)
#   PRE         seconds to wait after start before the driver (default 0)
#   POST        seconds to keep the game alive after the driver (default 0)
#
# Runs are serialised by one machine lock (private/.b3-mac-lock), shared with the build script,
# because this host has one GPU and both scripts rewrite/read the same binary.
# Output: $LOGS/<name>-game.log, $LOGS/<name>-drive.log, $LOGS/<name>-summary.txt
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"

NAME=${1:?usage: b3_mac_run.sh <name> <seconds> [KEY=VALUE ...]}
SECONDS_ARG=${2:-120}
shift 2 2>/dev/null || shift

EXE=${EXE:-$(b3_default_exe)}
DATA_ROOT=${DATA_ROOT:-$(b3_default_data_root)}
LOGS=${LOGS:-$(b3_default_logs)}
AUTODRIVE=${AUTODRIVE:-0}
EVERY=${EVERY:-1.5}
FRAMES=${FRAMES:-0}
PRE=${PRE:-0}
POST=${POST:-0}
TAKE_YES=${TAKE_YES:-0}
GAME_LOG=$LOGS/$NAME-game.log
DRIVE_LOG=$LOGS/$NAME-drive.log
SUMMARY=$LOGS/$NAME-summary.txt
# b3_export_base_env puts TMPDIR here (TMPDIR_OVERRIDE wins); keep the plan and the run in step.
BASE_TMPDIR=${TMPDIR_OVERRIDE:-$ROOT/private/tmp}
BASE_ENV="NX_DATA_ROOT=$DATA_ROOT NX_HEADLESS=1 NX_MUTE=1 NX_INPUT_LIVE=$DATA_ROOT/input_live.txt NX_PRESENT_TIMING=1 TMPDIR=$BASE_TMPDIR"
if [ -n "${AUDIO_DUMP:-}" ]; then BASE_ENV="$BASE_ENV NX_AUDIO_DUMP=$AUDIO_DUMP"; fi
if [ "$FRAMES" = "1" ]; then BASE_ENV="$BASE_ENV NX_DUMP_EVERY_S=${NX_DUMP_EVERY_S:-0.5}"; fi

# --- driver arguments (built here, executed through the contract) --------------------------------
build_drive_args() {
    # The option names below are b3_driver_args' own interface; the driver's argv is produced only
    # by b3_driver_args, so both hosts pass exactly the same flags.
    drive_args=(--root "$DATA_ROOT" --game-log "$GAME_LOG" --drive-log "$DRIVE_LOG" \
                --timeout "$SECONDS_ARG" --every "$EVERY" --a-only-until "${A_ONLY:-0}")
    if [ "$TAKE_YES" = "1" ]; then
        drive_args+=(--take-yes)
    fi
}
drive_args=()
build_drive_args

# --- dry run: print the plan, launch nothing -----------------------------------------------------
if b3_dry_run; then
    {
        printf 'entry: %s\n' "$ROOT/scripts/b3_mac_run.sh"
        printf 'platform: %s\n' "$(b3_platform)"
        printf 'commit: %s\n' "$(b3_git_commit)"
        printf 'name: %s\n' "$NAME"
        printf 'lock: private/.b3-mac-lock (mkdir; shared with scripts/b3_mac_build.sh)\n'
        printf 'game argv: env%s %s\n' "${*:+ $*}" "$EXE"
        printf 'env: %s\n' "$BASE_ENV"
        printf 'logging: game %s  driver %s  summary %s\n' "$GAME_LOG" "$DRIVE_LOG" "$SUMMARY"
        printf 'seconds: run %s  pre %s  post %s  every %s  frames %s\n' \
            "$SECONDS_ARG" "$PRE" "$POST" "$EVERY" "$FRAMES"
        # Diagnostic switches inherited from the caller. This entry point does not strip them (it is
        # the vehicle for switch experiments); scripts/b3_accept.sh strips them for a default run.
        for var in $(compgen -v 2>/dev/null | grep '^NX_' || true); do
            case " NX_DATA_ROOT NX_HEADLESS NX_MUTE NX_INPUT_LIVE NX_PRESENT_TIMING NX_AUDIO_DUMP NX_DUMP_EVERY_S NX_LOG NX_TMPDIR " in
                *" $var "*) continue ;;
            esac
            eval "val=\${$var}"
            printf 'diagnostic env: %s=%s\n' "$var" "$val"
        done
        # The driver argv is always shown: it is the contract, whether or not this run drives.
        if [ "$AUTODRIVE" = "1" ]; then
            printf 'driver: enabled (AUTODRIVE=1)\n'
        else
            printf 'driver: disabled (AUTODRIVE=0); the argv below is what AUTODRIVE=1 would run\n'
        fi
        printf 'driver argv: %s\n' "$(b3_driver_args "${drive_args[@]}" | tr '\n' ' ')"
    } | b3_dry_run_report
    exit 0
fi

mkdir -p "$LOGS"

# --- one machine at a time ----------------------------------------------------------------------
# b3_lock_acquire uses the same private/.b3-mac-lock directory the build script takes.
if ! b3_lock_acquire b3-mac 1800; then
    exit 1
fi
GAME_PID=""
DRIVE_PID=""
cleanup() {
    [ -n "$DRIVE_PID" ] && kill "$DRIVE_PID" 2>/dev/null
    [ -n "$GAME_PID" ] && kill "$GAME_PID" 2>/dev/null
    sleep 1
    pkill -x bayonetta3 2>/dev/null
    pkill -x ffmpeg 2>/dev/null
    b3_lock_release
}
trap cleanup EXIT

[[ -x "$EXE" ]] || { echo "error: no executable $EXE" >&2; exit 1; }
if pgrep -x bayonetta3 >/dev/null 2>&1; then
    echo "error: a bayonetta3 process is already alive:" >&2
    pgrep -x bayonetta3 >&2
    exit 1
fi

# Data root bootstrap (symlinks to the read-only inputs, local writes).
if [ ! -e "$DATA_ROOT/users.txt" ]; then
    SRC=${SRC_ROOT:-$(b3_ra2_root)/data}
    for required in "$SRC/analysis" "$SRC/run/extract" "$SRC/run-qm/spirv"; do
        [ -d "$required" ] || { echo "error: missing input $required; set SRC_ROOT or B3_RA2_ROOT" >&2; exit 1; }
    done
    [ -f "$SRC/ch-mov3/users.txt" ] || { echo "error: missing $SRC/ch-mov3/users.txt; set SRC_ROOT or B3_RA2_ROOT" >&2; exit 1; }
    mkdir -p "$DATA_ROOT/user/save" "$DATA_ROOT/frames"
    ln -sfn "$SRC/analysis" "$DATA_ROOT/analysis"
    ln -sfn "$SRC/run/extract" "$DATA_ROOT/extract"
    ln -sfn "$SRC/run-qm/spirv" "$DATA_ROOT/spirv"
    cp "$SRC/ch-mov3/users.txt" "$DATA_ROOT/users.txt"
    [ -f "$SRC/ch-mov3/user/save/GameData.dat" ] && cp "$SRC/ch-mov3/user/save/GameData.dat" "$DATA_ROOT/user/save/"
    cp "$SRC/ch-mov3/pipelines_106b_1b000209.bin" "$DATA_ROOT/" 2>/dev/null
    cp "$SRC/ch-mov3/pipelines_106b_1b000209.keys" "$DATA_ROOT/" 2>/dev/null
fi

# Repair missing input links when the configured source is available.
# The bootstrap above only runs for a fresh root, so an existing root needs this on its own.
SRC=${SRC_ROOT:-$(b3_ra2_root)/data}
for pair in "analysis=$SRC/analysis" "extract=$SRC/run/extract" "spirv=$SRC/run-qm/spirv"; do
    name=${pair%%=*}; target=${pair#*=}
    [ -d "$target" ] || continue
    if [ ! -e "$DATA_ROOT/$name" ]; then
        ln -sfn "$target" "$DATA_ROOT/$name"
        printf 'relinked %s -> %s\n' "$name" "$target"
    fi
done

# `extract` itself can resolve while `extract/romfs` inside it does not: the share stores that one as a
# *relative* link (../../romfs), so it survives a mount rename, but a root built by an older script has an
# absolute one pointing at the previous mount name. The game then never mounts Contents: and crashes in
# nnMain before the first frame, with a stack that looks like a code regression (it is not: the same
# binary boots fine on a good root). Check it here so the failure names itself.
if [ -e "$DATA_ROOT/extract" ] && [ ! -e "$DATA_ROOT/extract/romfs" ]; then
    if [ -d "$SRC/romfs" ]; then
        ln -sfn ../../romfs "$DATA_ROOT/extract/romfs"
        printf 'relinked extract/romfs -> ../../romfs (was dangling)\n'
    else
        printf 'error: %s/extract/romfs is a dangling link and %s/romfs is missing\n' "$DATA_ROOT" "$SRC" >&2
        exit 1
    fi
fi

# Leftover markers from an earlier run would start a trace/dump at boot.
b3_clear_run_markers "$DATA_ROOT"

b3_export_base_env "$DATA_ROOT"
if [ -n "${AUDIO_DUMP:-}" ]; then export NX_AUDIO_DUMP="$AUDIO_DUMP"; fi
if [ "$FRAMES" = "1" ]; then export NX_DUMP_EVERY_S=${NX_DUMP_EVERY_S:-0.5}; fi

# 9>&-: harmless without flock, and it keeps the lock fd out of the game's children if one exists.
env "$@" "$EXE" >"$GAME_LOG" 2>&1 9>&- &
GAME_PID=$!
sleep 2
if ! kill -0 "$GAME_PID" 2>/dev/null; then
    echo "error: the game exited during startup; see $GAME_LOG" >&2
    tail -20 "$GAME_LOG" >&2
    exit 1
fi

T0=$(date +%s)
if [ "$PRE" != "0" ]; then sleep "$PRE"; fi
if [ "$AUTODRIVE" = "1" ]; then
    b3_start_autodrive "${drive_args[@]}" >/dev/null
    DRIVE_PID=$B3_DRIVE_PID
fi
END=$((T0 + SECONDS_ARG + POST))
while [ "$(date +%s)" -lt "$END" ]; do
    sleep 2
    kill -0 "$GAME_PID" 2>/dev/null || break
done

# --- summary (the shared format: commit/platform, wall, FATAL/CRASH, frame windows, movies) ------
b3_write_summary "$SUMMARY" "$NAME" "$EXE" "$DATA_ROOT" "$(( $(date +%s) - T0 ))" "$GAME_LOG" "$*"
