#!/usr/bin/env bash
# Replay a recorded input script against the pinned save and measure what it reaches.
#
#   scripts/b3_replay.sh <name> <script file> [seconds] [KEY=VALUE ...]
#
# The script is the "<down>:<button>:<held>;" format NX_INPUT_RECORD writes and NX_INPUT_SCRIPT plays
# back, so a session a person played can be re-run exactly: the same boot, the same menu path, the
# same fight, with no autodrive guessing what the picture means. Wall-clock seconds are used, so the
# replay lands in the same place only if the frame rate is in the same class as the recorded run;
# check the frames it dumps before trusting a measurement from it.
#
#   SAVE_STATE   pinned save state to restore   (default logs/ch1/save-perfect/user)
#   DATA_ROOT    data root to run in            (default private/run-replay)
#   LOGS         where the game log goes        (default logs/ch1)
#   HEADED=1     a visible window instead of headless
#   FRAMES       frames to dump for a visual check (default 0: none)
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=lib/b3_common.sh
source "$ROOT/scripts/lib/b3_common.sh"

NAME=${1:?name}
SCRIPT_FILE=${2:?the recorded input script}
SECONDS_TO_RUN=${3:-240}
shift $(( $# < 3 ? $# : 3 ))

[[ -f "$SCRIPT_FILE" ]] || { printf 'error: no script %s\n' "$SCRIPT_FILE" >&2; exit 1; }
SAVE_STATE=${SAVE_STATE:-$ROOT/logs/ch1/save-perfect/user}
DATA_ROOT=${DATA_ROOT:-$ROOT/private/run-replay}
LOGS=${LOGS:-$(b3_default_logs)}
EXE=${EXE:-$ROOT/private/build-deck/bayonetta3}
mkdir -p "$LOGS"
GAME_LOG=$LOGS/$NAME-game.log

b3_lock_acquire b3-gpu || exit 1
trap 'b3_lock_release' EXIT
[[ -x "$EXE" ]] || { printf 'error: no executable %s\n' "$EXE" >&2; exit 1; }

# The pinned save, so the replay starts where the recording started.
if [[ -d "$SAVE_STATE" ]]; then
    rm -rf "$DATA_ROOT/user"
    cp -a "$SAVE_STATE" "$DATA_ROOT/user"
    printf 'replay: restored %s -> %s/user\n' "$SAVE_STATE" "$DATA_ROOT"
fi

b3_clear_run_markers "$DATA_ROOT"
b3_export_base_env "$DATA_ROOT"
export NX_AUDIO_DUMP="$DATA_ROOT/audio.wav"
[ "${HEADED:-0}" = "1" ] && unset NX_HEADLESS
[ "${FRAMES:-0}" -gt 0 ] 2>/dev/null && export NX_DUMP_FRAMES=$FRAMES
export NX_INPUT_SCRIPT
NX_INPUT_SCRIPT=$(cat "$SCRIPT_FILE")
printf 'replay: %s bytes of scripted input, running %ss\n' "$(wc -c < "$SCRIPT_FILE")" "$SECONDS_TO_RUN"

env "$@" "$EXE" >"$GAME_LOG" 2>&1 9>&- &
GAME_PID=$!
started=$(date +%s)
while kill -0 "$GAME_PID" 2>/dev/null; do
    if (( $(date +%s) - started >= SECONDS_TO_RUN )); then
        kill "$GAME_PID" 2>/dev/null
        sleep 3
        kill -9 "$GAME_PID" 2>/dev/null
        break
    fi
    sleep 2
done
wait "$GAME_PID" 2>/dev/null
printf 'replay: game stopped after %ss; log %s\n' "$(( $(date +%s) - started ))" "$GAME_LOG"

# The same budget line the battle harness prints, so the two are compared on one definition.
"$ROOT/scripts/b3_battle.sh" --extract "$NAME"
