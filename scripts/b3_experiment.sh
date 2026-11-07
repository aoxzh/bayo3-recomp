#!/usr/bin/env bash
# Run one unattended Bayonetta 3 experiment on Linux.
#
#   scripts/b3_experiment.sh <name> [drive seconds] [KEY=VALUE ...]
#
# Boots the release binary headless over DATA_ROOT (default private/run-vt, which has a warm pipeline
# cache and a save), drives it with the shared autodrive contract until the driver's timeout, keeps
# the game running POST more seconds so the frame budget can be read, then stops it. Writes
# logs/ch1/<name>-game.log and logs/ch1/<name>-drive.log and prints a summary.
#
# Everything that must not differ between the two hosts lives in scripts/lib/b3_common.sh: the run
# environment, the machine lock, the driver arguments and the clean-environment rule. This script
# uses the shared driver arguments so hosts do not silently diverge.
#
# Runs are serialised with the machine lock private/.b3-gpu-lock (flock on Linux): this machine
# shares one GPU and the fps numbers are only comparable when one game process runs at a time.
#
# B3_DRY_RUN=1 prints the assembled game/driver commands and exits without launching anything.
#
# Environment (before the KEY=VALUE list, which is passed to the game):
#   EXE        game binary        (default private/build-deck/bayonetta3)
#   DATA_ROOT  NX_DATA_ROOT       (default private/run-vt)
#   POST       seconds after the drive stops (default 60)
#   EVERY      autodrive poll seconds (default 1.5)
#   A_ONLY     seconds of A-only driving at the start (default 0; pair it with a boot A sequence)
#   TAKE_YES   1: the driver takes the YES option on YES/No pages (the completed-save hub's
#              chapter-start confirm needs it; off by default so the highlighted No is kept)
#   PERF      1: perf record the game for the last 60 s (needs the drive to reach a scene)
#   FRAMES    kept frames (default: autodrive's own on-demand dumps in the data root)
#   HEADED    1: open a real window and present through the Vulkan swapchain instead of the headless
#              path (which counts/dumps the presented texture and makes no swapchain). This is a
#              visual-evidence mode: it is NOT the default measurement environment, so an acceptance
#              record still comes from the headless run. Requires a display session.
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"

NAME=${1:?usage: b3_experiment.sh <name> [drive seconds] [KEY=VALUE ...]}
DRIVE_SECONDS=${2:-600}
shift 2 2>/dev/null || shift

EXE=${EXE:-$(b3_default_exe)}
DATA_ROOT=${DATA_ROOT:-$(b3_default_data_root)}
POST=${POST:-60}
EVERY=${EVERY:-1.5}
PERF=${PERF:-0}
HEADED=${HEADED:-0}
case "$HEADED" in
    0|1) ;;
    *) printf 'error: HEADED must be 0 or 1 (got %s)\n' "$HEADED" >&2; exit 2 ;;
esac
LOGS=${LOGS:-$(b3_default_logs)}
GAME_LOG=$LOGS/$NAME-game.log
DRIVE_LOG=$LOGS/$NAME-drive.log
SUMMARY=$LOGS/$NAME-summary.txt
# b3_export_base_env puts TMPDIR here (TMPDIR_OVERRIDE wins); keep the plan and the run in step.
BASE_TMPDIR=${TMPDIR_OVERRIDE:-$ROOT/private/tmp}
if [ "$HEADED" = "1" ]; then
    BASE_ENV="NX_DATA_ROOT=$DATA_ROOT NX_MUTE=1 NX_INPUT_LIVE=$DATA_ROOT/input_live.txt NX_PRESENT_TIMING=1 NX_AUDIO_DUMP=$DATA_ROOT/audio.wav TMPDIR=$BASE_TMPDIR"
else
    BASE_ENV="NX_DATA_ROOT=$DATA_ROOT NX_HEADLESS=1 NX_MUTE=1 NX_INPUT_LIVE=$DATA_ROOT/input_live.txt NX_PRESENT_TIMING=1 NX_AUDIO_DUMP=$DATA_ROOT/audio.wav TMPDIR=$BASE_TMPDIR"
fi

# --- driver arguments (built here, executed through the contract) --------------------------------
build_drive_args() {
    # The option names below are b3_driver_args' own interface; the driver's argv is produced only
    # by b3_driver_args, so both hosts pass exactly the same flags.
    drive_args=(--root "$DATA_ROOT" --game-log "$GAME_LOG" --drive-log "$DRIVE_LOG" \
                --timeout "$DRIVE_SECONDS" --every "$EVERY" --a-only-until "${A_ONLY:-0}")
    if [ "${TAKE_YES:-0}" = "1" ]; then
        drive_args+=(--take-yes)
    fi
}
drive_args=()
build_drive_args

# --- dry run: print the plan, launch nothing -----------------------------------------------------
if b3_dry_run; then
    {
        printf 'entry: %s\n' "$ROOT/scripts/b3_experiment.sh"
        printf 'platform: %s\n' "$(b3_platform)"
        printf 'commit: %s\n' "$(b3_git_commit)"
        printf 'name: %s\n' "$NAME"
        printf 'window: %s\n' "$([ "$HEADED" = "1" ] && printf headed || printf headless)"
        printf 'lock: private/.b3-gpu-lock (flock where available)\n'
        printf 'game argv: env%s %s\n' "${*:+ $*}" "$EXE"
        printf 'env: %s\n' "$BASE_ENV"
        printf 'logging: game %s  driver %s  summary %s\n' "$GAME_LOG" "$DRIVE_LOG" "$SUMMARY"
        printf 'seconds: drive %s  post %s  every %s  perf %s\n' \
            "$DRIVE_SECONDS" "$POST" "$EVERY" "$PERF"
        # Diagnostic switches inherited from the caller. This entry point does not strip them (it is
        # the vehicle for switch experiments); scripts/b3_accept.sh strips them for a default run.
        for var in $(compgen -v 2>/dev/null | grep '^NX_' || true); do
            case " NX_DATA_ROOT NX_HEADLESS NX_MUTE NX_INPUT_LIVE NX_PRESENT_TIMING NX_AUDIO_DUMP NX_LOG NX_TMPDIR " in
                *" $var "*) continue ;;
            esac
            eval "val=\${$var}"
            printf 'diagnostic env: %s=%s\n' "$var" "$val"
        done
        printf 'driver argv: %s\n' "$(b3_driver_args "${drive_args[@]}" | tr '\n' ' ')"
    } | b3_dry_run_report
    exit 0
fi

mkdir -p "$LOGS"

# --- one game at a time -------------------------------------------------------------------------
if ! b3_lock_acquire b3-gpu 7200; then
    exit 1
fi
trap 'b3_lock_release' EXIT

if [ -n "$(b3_game_pids "$EXE")" ]; then
    printf 'error: a game process from another run is alive:\n' >&2
    pgrep -af "$EXE" >&2
    exit 1
fi
[[ -x "$EXE" ]] || { printf 'error: no executable %s\n' "$EXE" >&2; exit 1; }

# Leftover markers from an earlier run would start a trace/dump at boot.
b3_clear_run_markers "$DATA_ROOT"

b3_export_base_env "$DATA_ROOT"
export NX_AUDIO_DUMP="$DATA_ROOT/audio.wav"
# HEADED=1: b3_export_base_env is the headless measurement environment; drop its NX_HEADLESS so
# create_window() makes a visible window and presenting takes the Vulkan swapchain path.
[ "$HEADED" = "1" ] && unset NX_HEADLESS

# The KEY=VALUE list is the caller's switches, still allowed here. 9>&-: the game must not inherit
# the lock fd, or the lock lives as long as its ffmpeg children.
env "$@" "$EXE" >"$GAME_LOG" 2>&1 9>&- &
GAME_PID=$!
sleep 2
if ! kill -0 "$GAME_PID" 2>/dev/null; then
    printf 'error: the game exited during startup; see %s\n' "$GAME_LOG" >&2
    tail -30 "$GAME_LOG" >&2
    exit 1
fi

B3_DRIVE_STDERR=$LOGS/$NAME-drive-stderr.log
b3_start_autodrive "${drive_args[@]}" >/dev/null
DRIVE_PID=$B3_DRIVE_PID
DRIVE_STATUS=running
DRIVE_STOPPED_BY_HARNESS=0
run_rc=0

started=$(date +%s)
phase=drive
while :; do
    now=$(date +%s)
    if ! kill -0 "$GAME_PID" 2>/dev/null; then
        printf 'game exited during %s after %ss\n' "$phase" "$((now - started))"
        run_rc=1
        break
    fi
    if [[ $phase == drive ]] && ! kill -0 "$DRIVE_PID" 2>/dev/null; then
        if wait "$DRIVE_PID"; then DRIVE_STATUS=0; else DRIVE_STATUS=$?; fi
        DRIVE_PID=
        printf 'drive exited %s after %ss: %s\n' "$DRIVE_STATUS" "$((now - started))" "$(tail -1 "$DRIVE_LOG" 2>/dev/null)"
        if [ "$DRIVE_STATUS" -ne 0 ]; then
            printf 'error: autodrive exited %s; see %s\n' "$DRIVE_STATUS" "$B3_DRIVE_STDERR" >&2
            run_rc=1
            break
        fi
        phase=post
        started=$(date +%s)
    fi
    if [[ $phase == post && $((now - started)) -ge $POST ]]; then
        break
    fi
    if [[ $PERF == 1 && $phase == post && -z "${PERF_PID:-}" ]]; then
        perf record -q -F 199 -g -p "$GAME_PID" -o "$LOGS/$NAME.perf" -- sleep 45 >/dev/null 2>&1 &
        PERF_PID=$!
    fi
    sleep 1
done

if [[ -n ${DRIVE_PID:-} ]]; then
    if kill -0 "$DRIVE_PID" 2>/dev/null; then
        DRIVE_STOPPED_BY_HARNESS=1
        kill "$DRIVE_PID" 2>/dev/null || true
    fi
    if wait "$DRIVE_PID"; then DRIVE_STATUS=0; else DRIVE_STATUS=$?; fi
    DRIVE_PID=
fi
b3_clear_run_markers "$DATA_ROOT"
if kill -0 "$GAME_PID" 2>/dev/null; then
    kill -TERM "$GAME_PID" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$GAME_PID" 2>/dev/null || break; sleep 0.5; done
    kill -KILL "$GAME_PID" 2>/dev/null
fi
wait "$GAME_PID" 2>/dev/null
[[ -n ${PERF_PID:-} ]] && wait "$PERF_PID" 2>/dev/null

# --- summary ------------------------------------------------------------------------------------
{
    printf 'experiment %s  binary %s  data root %s\n' "$NAME" "$EXE" "$DATA_ROOT"
    printf 'commit: %s\n' "$(b3_git_commit)"
    printf 'platform: %s\n' "$(b3_platform)"
    printf 'window: %s\n' "$([ "$HEADED" = "1" ] && printf headed || printf headless)"
    printf 'extra env: %s\n' "$*"
    printf 'driver exit: %s  stopped by harness: %s  stderr: %s\n' "$DRIVE_STATUS" "$DRIVE_STOPPED_BY_HARNESS" "$B3_DRIVE_STDERR"
    printf 'FATAL: %s  CRASH: %s\n' "$(grep -c FATAL "$GAME_LOG")" "$(grep -c 'CRASH:' "$GAME_LOG")"
    # Headed runs take the swapchain path; headless runs never make one, so these lines only exist
    # when HEADED=1 and their presence is the evidence that a real window presented.
    printf 'swapchain:\n'
    grep -E 'nvn/vk: (present mode|swapchain)' "$GAME_LOG" | tail -4
    printf 'movies:\n'
    grep -E 'movie:|WriteTexels' "$GAME_LOG" | tail -8
    printf 'chapter markers:\n'
    grep -E 'Chapter|chapter|RESULT|am: ' "$GAME_LOG" | tail -4
    printf 'frame windows: %s\n' "$(grep -c 'per frame over 60' "$GAME_LOG")"
    grep 'per frame over 60' "$GAME_LOG" | tail -120 > "$LOGS/$NAME-windows.txt"
    "$(b3_python)" - "$LOGS/$NAME-windows.txt" <<'PY'
import re, statistics, sys
rows = []
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'([\d.]+) fps, ([\d.]+) submits, replay ([\d.]+) ms, submit \(encode\) ([\d.]+) ms, '
                  r'fence wait ([\d.]+) ms, other ([\d.]+) ms, present ([\d.]+) ms, upload ([\d.]+) MB', line)
    if m:
        rows.append([float(x) for x in m.groups()])
if rows:
    cols = list(zip(*rows))
    fps = cols[0]
    print(f'windows {len(rows)}  fps median {statistics.median(fps):.1f} min {min(fps):.1f} max {max(fps):.1f} '
          f'under30 {sum(1 for f in fps if f < 30)}')
    names = ['fps', 'submits', 'replay', 'encode', 'fence', 'other', 'present', 'upload']
    print('median ' + '  '.join(f'{n} {statistics.median(c):.2f}' for n, c in zip(names, cols)))
else:
    print('no frame windows')
PY
    printf 'last drive decisions:\n'
    tail -6 "$DRIVE_LOG" 2>/dev/null
} | tee "$SUMMARY"
printf 'logs: %s\n' "$GAME_LOG"
exit "$run_rc"
