#!/usr/bin/env bash
# Quick frame-rate benchmark for one Linux configuration.
#
#   scripts/b3_bench.sh <name> [seconds] [KEY=VALUE ...]
#
# Boots the release binary headless on DATA_ROOT, lets it settle into the 3D scene the title screen
# shows (~10 s after start, and it is the same heavy path as gameplay), collects the frame windows,
# then stops the game and prints fps/replay/fence statistics. No autodrive, no frame dumps: this
# measures rendering, nothing else. Runs are serialised on private/.b3-gpu-lock.
#
#   EXE        game binary   (default private/build-deck/bayonetta3)
#   DATA_ROOT  NX_DATA_ROOT  (default private/run-vt)
#   SKIP       first N frame windows to drop (default 4: boot, title load, logo)
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"
NAME=${1:?usage: b3_bench.sh <name> [seconds] [KEY=VALUE ...]}
SECONDS_RUN=${2:-75}
shift 2 2>/dev/null || shift

EXE=${EXE:-$ROOT/private/build-deck/bayonetta3}
DATA_ROOT=${DATA_ROOT:-$ROOT/private/run-vt}
SKIP=${SKIP:-4}
LOGS=${LOGS:-$ROOT/logs/ch1}
GAME_LOG=$LOGS/$NAME-game.log
WINDOWS=$LOGS/$NAME-windows.txt
SUMMARY=$LOGS/$NAME-summary.txt
mkdir -p "$LOGS" "$ROOT/private/tmp"

exec 9>"$ROOT/private/.b3-gpu-lock"
if ! flock -w 7200 9; then
    printf 'error: another run holds the GPU lock\n' >&2
    exit 1
fi
if [ -n "$(b3_game_pids "$EXE")" ]; then
    printf 'error: a game from another run is alive:\n' >&2
    pgrep -af "$EXE" >&2
    exit 1
fi
[[ -x "$EXE" ]] || { printf 'error: no executable %s\n' "$EXE" >&2; exit 1; }

rm -f "$DATA_ROOT/dump_frames" "$DATA_ROOT/trace_present" "$DATA_ROOT/autodrive_run"
: > "$DATA_ROOT/input_live.txt"
export NX_DATA_ROOT="$DATA_ROOT" TMPDIR="$ROOT/private/tmp"
export NX_HEADLESS=1 NX_MUTE=1 NX_PRESENT_TIMING=1
# 9>&-: the game must not inherit the flock fd, or the lock lives as long as its ffmpeg children.
env "$@" "$EXE" >"$GAME_LOG" 2>&1 9>&- &
PID=$!
started=$(date +%s)
while kill -0 "$PID" 2>/dev/null && (( $(date +%s) - started < SECONDS_RUN )); do sleep 1; done
: > "$DATA_ROOT/input_live.txt"
kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
kill -KILL "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

grep 'per frame over 60' "$GAME_LOG" | tail -n +$((SKIP + 1)) > "$WINDOWS"
{
    printf 'bench %s  seconds %s  binary %s  data root %s\n' "$NAME" "$SECONDS_RUN" "$EXE" "$DATA_ROOT"
    printf 'extra env: %s\n' "$*"
    printf 'FATAL: %s  CRASH: %s\n' "$(grep -c FATAL "$GAME_LOG")" "$(grep -c 'CRASH:' "$GAME_LOG")"
    python3 - "$WINDOWS" <<'PY'
import re, statistics, sys
rows = []
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'([\d.]+) fps, ([\d.]+) submits, replay ([\d.]+) ms, submit \(encode\) ([\d.]+) ms, '
                  r'fence wait ([\d.]+) ms, other ([\d.]+) ms, present ([\d.]+) ms, upload ([\d.]+) MB', line)
    if m:
        rows.append([float(x) for x in m.groups()])
if not rows:
    print('no frame windows'); sys.exit()
cols = list(zip(*rows))
fps = cols[0]
q = sorted(fps)
def pct(p): return q[min(len(q) - 1, int(p * len(q)))]
print(f'windows {len(rows)}  fps median {statistics.median(fps):.1f}  p10 {pct(0.1):.1f}  p90 {pct(0.9):.1f}  '
      f'min {min(fps):.1f}  max {max(fps):.1f}  under30 {sum(1 for f in fps if f < 30)}')
names = ['fps', 'submits', 'replay', 'encode', 'fence', 'other', 'present', 'upload']
print('median  ' + '  '.join(f'{n} {statistics.median(c):.2f}' for n, c in zip(names, cols)))
PY
    grep -c 'per frame over 60' "$GAME_LOG" | sed 's/^/total windows incl. boot: /'
} | tee "$SUMMARY"
