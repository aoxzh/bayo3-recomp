#!/usr/bin/env bash
# One unattended Bayonetta 3 run on the Windows host.
#
#   scripts/b3_win_run.sh <name> <seconds> [KEY=VALUE ...]
#
# Same contract as the Deck's scripts/b3_experiment.sh and the Mac's scripts/b3_mac_run.sh
# (scripts/lib/b3_common.sh): one definition of the run environment, of the machine lock and of the
# driver arguments, so a flag cannot be passed on one host and silently missing on the other.
#
# The Windows host differs in three ways that this script owns:
#   * there is no /proc, so the "is a game already running" check goes through tasklist;
#   * links are NTFS junctions (mklink /J), not symlinks: a plain symlink needs developer mode or an
#     elevated shell, a junction needs neither;
#   * the console window is what a player sees, so the default here is windowed; NX_HEADLESS=1 is
#     the measurement mode (the Mac and Deck scripts default the other way round because they are
#     driven over ssh, where a window is not available at all).
#
# Environment:
#   EXE         game binary            (default private/build-win/bayonetta3.exe)
#   DATA_ROOT   NX_DATA_ROOT           (default private/run-win)
#   LOGS        log dir                (default <repo>/logs/win)
#   GAME_ROOT   the read-only inputs the data root is linked to (ExeFS' parent: RomFS, base, update)
#   SPIRV_DIR   translated shaders     (default private/bayonetta3-shaders-tess-hmflower-v1/spirv)
#   AUTODRIVE   1: run the shared autodrive for the same <seconds> (default 0: just run)
#   EVERY       autodrive poll seconds (default 1.5)
#   TAKE_YES    passed through to the driver (see b3_mac_run.sh)
#   PRE, POST   seconds around the driver (default 0)
#   WINDOWED    1: keep the window (default 1); 0: NX_HEADLESS=1
#
# Output: $LOGS/<name>-game.log, $LOGS/<name>-drive.log, $LOGS/<name>-summary.txt
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"

NAME=${1:?usage: b3_win_run.sh <name> <seconds> [KEY=VALUE ...]}
SECONDS_ARG=${2:-120}
shift 2 2>/dev/null || shift

EXE=${EXE:-$(b3_default_exe)}
DATA_ROOT=${DATA_ROOT:-$(b3_default_data_root)}
LOGS=${LOGS:-$(b3_default_logs)}
AUTODRIVE=${AUTODRIVE:-0}
EVERY=${EVERY:-1.5}
PRE=${PRE:-0}
POST=${POST:-0}
TAKE_YES=${TAKE_YES:-0}
WINDOWED=${WINDOWED:-1}
GAME_LOG=$LOGS/$NAME-game.log
DRIVE_LOG=$LOGS/$NAME-drive.log
SUMMARY=$LOGS/$NAME-summary.txt
EXE_WIN=$(b3_win_path "$EXE")
DATA_ROOT_WIN=$(b3_win_path "$DATA_ROOT")

# The base environment a measurement run needs. Keep this in step with the dry run below.
base_env=(
    "NX_DATA_ROOT=$DATA_ROOT_WIN"
    "NX_INPUT_LIVE=$DATA_ROOT_WIN\\input_live.txt"
    "NX_PRESENT_TIMING=1"
)
if [ "$WINDOWED" = "1" ]; then
    base_env+=("NX_FPS_OVERLAY=1")
else
    base_env+=("NX_HEADLESS=1" "NX_MUTE=1")
fi

# --- driver arguments (built here, executed through the contract) --------------------------------
build_drive_args() {
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
        printf 'entry: %s\n' "$ROOT/scripts/b3_win_run.sh"
        printf 'platform: %s\n' "$(b3_platform)"
        printf 'commit: %s\n' "$(b3_git_commit)"
        printf 'name: %s\n' "$NAME"
        printf 'lock: private/.b3-win-lock\n'
        printf 'game argv: env%s %s\n' "${*:+ $*}" "$EXE_WIN"
        printf 'env: %s\n' "${base_env[*]}"
        printf 'logging: game %s  driver %s  summary %s\n' "$GAME_LOG" "$DRIVE_LOG" "$SUMMARY"
        printf 'seconds: run %s  pre %s  post %s  every %s\n' "$SECONDS_ARG" "$PRE" "$POST" "$EVERY"
        for var in $(compgen -v 2>/dev/null | grep '^NX_' || true); do
            case " NX_DATA_ROOT NX_INPUT_LIVE NX_PRESENT_TIMING NX_FPS_OVERLAY NX_HEADLESS NX_MUTE NX_LOG NX_TMPDIR " in
                *" $var "*) continue ;;
            esac
            eval "val=\${$var}"
            printf 'diagnostic env: %s=%s\n' "$var" "$val"
        done
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
if ! b3_lock_acquire b3-win 1800; then
    exit 1
fi
GAME_PID=""
DRIVE_PID=""
cleanup() {
    [ -n "$DRIVE_PID" ] && kill "$DRIVE_PID" 2>/dev/null
    [ -n "$GAME_PID" ] && kill "$GAME_PID" 2>/dev/null
    sleep 1
    b3_win_kill_game
    b3_lock_release
}
trap cleanup EXIT

[[ -x "$EXE" ]] || { echo "error: no executable $EXE" >&2; exit 1; }
if b3_win_game_pids | grep -q .; then
    echo "error: a bayonetta3 process is already alive:" >&2
    b3_win_game_pids >&2
    exit 1
fi

# --- data root: junctions to the read-only inputs, local writes ----------------------------------
b3_win_link_data_root "$DATA_ROOT"

# Leftover markers from an earlier run would start a trace/dump at boot.
: > "$DATA_ROOT/input_live.txt"

LOG_WIN=$(b3_win_path "$GAME_LOG")
env "${base_env[@]}" "NX_LOG=$LOG_WIN" "$EXE_WIN" "$@" >"$GAME_LOG" 2>&1 &
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
