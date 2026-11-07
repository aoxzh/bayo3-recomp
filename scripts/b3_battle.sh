#!/usr/bin/env bash
# Battle-state benchmark: the Prologue fight scene, measured from a pinned save state.
#
#   scripts/b3_battle.sh <name> [drive seconds] [KEY=VALUE ...]
#   scripts/b3_battle.sh --extract <name>          # re-print the budget of an existing run
#
# What it does, in order:
#   1. restores the pinned save state (logs/ch1/battle-state/user, the state that reaches the fight)
#      into $DATA_ROOT/user, so every run starts from the same save;
#   2. runs scripts/b3_experiment.sh (default environment unless KEY=VALUE switches are given; that
#      script takes the machine lock itself, so never wrap this one in another flock);
#   3. extracts ONLY the heavy windows from the run's tail and prints the frame budget:
#      window count, fps median/p10/min, replay/encode/fence/other/present/upload medians, the
#      write-back wall, the compute-ranges line, submit timing, and (when the run was profiled) the
#      NX_GPU_PROFILE=3 `gpu busy` line and the NX_GPU_PROFILE=1 per-pass breakdown.
#
# BATTLE candidate: a 60-frame window with upload > 50 MB, regardless of FPS. The wide fallback
# uses upload > 35 MB. Workload, driver/freeze evidence and the descriptor gate below identify the
# fight; FPS is only an outcome. The benchmark reports the largest contiguous stretch (a gap of up
# to 2 windows is tolerated). Fewer than $BATTLE_MIN_WINDOWS (default 8) is a failed run: exit 1
# with the evidence. Preserved frames must still be checked to confirm actual combat.
#
# Environment:
#   DATA_ROOT    data root to run in           (default private/run-movie)
#   SAVE_STATE   pinned save state to restore  (default logs/ch1/battle-state/user)
#   LOGS         log directory                 (default logs/ch1)
#   BATTLE_MIN_WINDOWS  battle windows required (default 8)
#   GPU_SAMPLE   1: sample /sys/class/drm/card0/device/gpu_busy_percent every 0.2 s into
#                logs/ch1/<name>-gpu.csv while the game runs (an independent check on "is the GPU
#                idle in this scene"; the amdgpu value is 0 or 99 per sample, so report its mean).
#   B3_MOVIE_SKIP_AFTER  driver movie-skip timer (default 20 here; the driver's own default is 150).
#                The Prologue cutscene chain (ev0020/21/22/23/25) runs ~650 s at 150 s; skipping all
#                long films at 20 s takes the chain to ~100 s and leaves the drive for the fight. This
#                is a *driver* knob: the game env is the default one. Set 150 to reproduce the old
#                chain.
#   Everything else (EXE, POST, PERF, HEADED) is passed through to scripts/b3_experiment.sh.
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"

extract_only=0
if [ "${1:-}" = "--extract" ]; then
    extract_only=1
    shift
fi
NAME=${1:?usage: b3_battle.sh <name> [drive seconds] [KEY=VALUE ...] | b3_battle.sh --extract <name>}
shift
DRIVE_SECONDS=${1:-700}
[ $# -gt 0 ] && shift

DATA_ROOT=${DATA_ROOT:-private/run-movie}
EXE=${EXE:-$(b3_default_exe)}
SAVE_STATE=${SAVE_STATE:-$ROOT/logs/ch1/battle-state/user}
LOGS=${LOGS:-$(b3_default_logs)}
GAME_LOG=$LOGS/$NAME-game.log
DRIVE_LOG=$LOGS/$NAME-drive.log
BATTLE_OUT=$LOGS/$NAME-battle.txt
BATTLE_MIN_WINDOWS=${BATTLE_MIN_WINDOWS:-8}
# Descriptor-set peaks are scene-specific: the Prologue fight binds ~1,800-3,000 per submit, the Chapter 2
# fight ~1,820-1,890, while movie/menu states stay under ~250 and hub/street exploration under ~1,250. The
# gate is a "is this the combat state" check against that scene's own calibrated value, never against a
# threshold borrowed from another chapter.
BATTLE_MIN_SETS=${BATTLE_MIN_SETS:-2000}
# Upload traffic is a valid fight signal only where the scene streams little else. The Chapter 2 street
# streams 55-75 MB/frame while merely walking, so there the descriptor peak is the state signal and the
# upload thresholds are lowered (BATTLE_STRICT_MB / BATTLE_WIDE_MB / BATTLE_HEAVY_MB).
BATTLE_STRICT_MB=${BATTLE_STRICT_MB:-50}
BATTLE_WIDE_MB=${BATTLE_WIDE_MB:-35}
BATTLE_HEAVY_MB=${BATTLE_HEAVY_MB:-60}
mkdir -p "$LOGS"

extract() {  # $1: game log, $2: drive log (may be missing), $3: output prefix ("" = stdout only) $4: gpu csv
    "$(b3_python)" - "$1" "${2:-}" "$3" "$BATTLE_MIN_WINDOWS" "${BATTLE_MIN_SETS:-2000}" "$BATTLE_STRICT_MB" "$BATTLE_WIDE_MB" "$BATTLE_HEAVY_MB" "${4:-}" <<'PY'
import re, statistics, sys

game_log, drive_log, out_prefix, min_windows, min_sets = (sys.argv[1], sys.argv[2], sys.argv[3],
                                                         int(sys.argv[4]), int(sys.argv[5]))
strict_mb, wide_mb, heavy_mb, gpu_csv = float(sys.argv[6]), float(sys.argv[7]), float(sys.argv[8]), sys.argv[9]
lines = open(game_log, errors='replace').read().splitlines()

win_re = re.compile(r'per frame over 60: ([\d.]+) fps, ([\d.]+) submits, replay ([\d.]+) ms, '
                    r'submit \(encode\) ([\d.]+) ms, fence wait ([\d.]+) ms, other ([\d.]+) ms, '
                    r'present ([\d.]+) ms, upload ([\d.]+) MB')
wall_re = re.compile(r'write-back wall ([\d.]+) ms/frame per batch \(([\d.]+) ranges, ([\d.]+) MB')
snap_re = re.compile(r'write-back snapshots.*?([\d.]+) ms/frame.*?([\d.]+) MB/frame')
chunk_re = re.compile(r'write-back chunks .*')
sub_re = re.compile(r'submit timing over 60: ([\d.]+) fps, period ([\d.]+) ms, guest-thread cpu ([\d.]+) ms, '
                    r'call ([\d.]+) ms')
busy_re = re.compile(r'gpu busy: ([\d.]+) ms/frame \(([\d.]+) parts/frame\), span ([\d.]+) ms/frame')
pipe_re = re.compile(r'pipelined: ')
comp_re = re.compile(r'compute ranges: ')
wb_re = re.compile(r'write-backs: ')
pool_re = re.compile(r'descriptor pool peak per submit: (\d+) sets')
draws_re = re.compile(r'nvn draws: (\d+) recorded, (\d+) replayed')
sets_re = re.compile(r"nvn: descriptor sets: (\d+) reused from the submit's cache, (\d+) made")
thread_re = re.compile(r'replay thread over 60: ([\d.]+) jobs/frame, busy ([\d.]+) ms/frame, idle ([\d.]+) ms/frame')

rows = []
# Workload per window comes from the aux counters. They span a variable number of windows: the first
# blocks cover 60 presents each (1 window, cumulative 60/120/180/240/300) and every later block covers
# 300 presents (5 windows). nvn_vk.cpp present_headless calls draw_log_stats() when `presents` hits
# 60/120/180/240/300 and then every 300, before log_timing(60) prints the window line. `nvn draws:` is
# cumulative, so a block's work is the delta; descriptor sets are already per block. The workload
# normalisation is what makes replay cost comparable between arms whose fps differs.
pending_draws, pending_sets, last_draws = None, None, None
aux_at = []        # (row index assigned so far, per-frame draws, per-frame sets)
for i, l in enumerate(lines):
    m = draws_re.search(l)
    if m:
        cur = int(m.group(2))
        pending_draws = cur if last_draws is None else max(0, cur - last_draws)
        last_draws = cur
        continue
    m = sets_re.search(l)
    if m:
        pending_sets = int(m.group(2))
        continue
    m = win_re.search(l)
    if m:
        fps, submits, replay, encode, fence, other, present, upload = (float(x) for x in m.groups())
        rows.append(dict(line=i, fps=fps, submits=submits, replay=replay, encode=encode, fence=fence,
                         other=other, present=present, upload=upload,
                         draws=None, sets=None, busy=float('nan'), idle=float('nan')))
        if pending_draws is not None:
            aux_at.append((len(rows) - 1, pending_draws, pending_sets))
            pending_draws, pending_sets = None, None
        continue
# nvn_vk.cpp:647 prints worker_log() right AFTER the window line of the same log_timing() call, so the
# replay-thread figures describe the window that line just reported, not the next one.
for i, l in enumerate(lines):
    m = thread_re.search(l)
    if not m:
        continue
    prev = [r for r in rows if r['line'] < i]
    if prev:
        prev[-1]['busy'], prev[-1]['idle'] = float(m.group(2)), float(m.group(3))
# Spread each block over the windows it covers: N = rows since the previous block's end window.
covered_from = 0
for k, (at, draws, sets) in enumerate(aux_at):
    span = [r for r in rows[covered_from:at + 1]]
    covered_from = at + 1
    if not span:
        continue
    draws_pf = draws / (60.0 * len(span))
    # Both counters cover the same presents as the draws: `descriptor sets: N reused, M made` is reset by
    # sets_log() at the same cadence, so it needs the same 60-presents-per-window division.
    sets_pf = (sets / (60.0 * len(span))) if sets is not None else None
    for r in span:
        r['draws'], r['sets'] = draws_pf, sets_pf
blocks = []   # (log line of the window that closes the block, presents the block covers)
prev_at = -1
for at, _d, _s in aux_at:
    blocks.append((rows[at]['line'], 60 * (at - prev_at)))
    prev_at = at

out = []

def emit(s=''):
    out.append(s)

def finish(code):
    # Every exit writes what was collected: a failed run must still leave its evidence in the file.
    text = '\n'.join(out)
    sys.stdout.write(text + '\n')
    if out_prefix:
        open(out_prefix, 'w').write(text + '\n')
    sys.exit(code)

def span_presents(at_line):
    """Presents covered by the aux-counter block that begins before at_line.

    nvn_vk.cpp calls draw_log_stats() on present 60/120/180/240/300 and then every 300, so the first five
    blocks are one window each and later ones five. The aux line precedes the window line that closes its
    block, so pick the first block whose closing window comes after at_line."""
    idx = at_line if isinstance(at_line, int) else next(
        (i for i, line in enumerate(lines) if line is at_line or line == at_line), None)
    if idx is None:
        return 300
    for end_line, presents in blocks:
        if end_line >= idx:
            return presents
    return 300


if not rows:
    emit('battle: FAIL no frame windows in %s' % game_log)
    finish(1)

# Battle candidates are selected by workload, independently of FPS. Two upload thresholds keep
# improved runs comparable even when an optimization reduces upload traffic. Driver/freeze and
# descriptor evidence below reject known noncombat states; image review remains necessary.
STRICT_UPLOAD, WIDE_UPLOAD, HEAVY_UPLOAD = strict_mb, wide_mb, heavy_mb
def battle_keys(upload_min):
    return [k for k, r in enumerate(rows) if r['upload'] > upload_min]
def clusters(keys):  # contiguous stretches, a gap of up to 2 windows tolerated
    out = []
    for k in keys:
        if out and k - out[-1][-1] <= 3:
            out[-1].append(k)
        else:
            out.append([k])
    return out
strict_keys, wide_keys = battle_keys(STRICT_UPLOAD), battle_keys(WIDE_UPLOAD)
heavy_keys = battle_keys(HEAVY_UPLOAD)
if not wide_keys:
    emit('battle: FAIL 0 battle windows (upload > %.0f MB) in %d windows: fps min %.1f, '
         'upload max %.1f MB' % (WIDE_UPLOAD, len(rows), min(r['fps'] for r in rows), max(r['upload'] for r in rows)))
    finish(1)
strict_clusters, wide_clusters = clusters(strict_keys), clusters(wide_keys)
heavy_clusters = clusters(heavy_keys)
pool = strict_clusters or wide_clusters
block = max(pool, key=len)   # the largest contiguous stretch, not the last one: the fight's phases
sel = [rows[k] for k in block]
first_line, last_line = rows[block[0]]['line'], rows[block[-1]]['line']
fps = [r['fps'] for r in sel]
sfps = sorted(fps)
p10 = sfps[min(len(sfps) - 1, int(round(0.10 * (len(sfps) - 1))))]

def med(key):
    v = [r[key] for r in sel if key in r and r[key] is not None and r[key] == r[key]]
    return statistics.median(v) if v else float('nan')

emit('battle: %s' % game_log)
emit('windows: %d total; %d battle windows (upload > %.0f MB), %d (upload > %.0f MB); '
     'reporting the largest contiguous stretch of %d (log lines %d-%d)'
     % (len(rows), len(strict_keys), STRICT_UPLOAD, len(wide_keys), WIDE_UPLOAD, len(sel),
        first_line + 1, last_line + 1))
if len(sel) < min_windows:
    emit('battle: FAIL only %d heavy windows (need %d): the run did not reach/keep the fight state'
         % (len(sel), min_windows))
    finish(1)
emit('fps: median %.1f  p10 %.1f  min %.1f  max %.1f   (60-frame windows, %d frames/window)'
     % (statistics.median(fps), p10, min(fps), max(fps), 60))
wall = sum(60.0 / f for f in fps)
emit('heavy span: %.1f s of game time at the measured fps' % wall)
emit('per-window medians: submits %.2f  replay %.2f  encode %.2f  fence-wait %.2f  other %.2f  present %.2f  upload %.2f MB'
     % (med('submits'), med('replay'), med('encode'), med('fence'), med('other'), med('present'), med('upload')))
# Workload-normalized cost: the frame-to-frame fps spread between identical configurations is dominated
# by the guest/replay hand-off (replay-thread idle), so replay is divided by the work it recorded
# (replayed draws) and by the descriptor-set traffic. Judges a CPU cut on the work, not on the fps.
wl = [r for r in sel if r['draws'] is not None]
if wl:
    draws_pf = statistics.median(r['draws'] for r in wl)   # already per frame for its own block
    sets_wl = [r for r in wl if r['sets'] is not None]
    sets_pf = statistics.median(r['sets'] for r in sets_wl) if sets_wl else float('nan')
    ns_draw = med('replay') * 1e6 / draws_pf if draws_pf else float('nan')
    emit('cost per draw: replay %.2f ms / %.0f replayed draws per frame = %.0f ns/draw (%.1f us); '
         '%.0f descriptor sets made per frame -> %.1f ns/set'
         % (med('replay'), draws_pf, ns_draw, ns_draw / 1000.0, sets_pf,
            (med('replay') * 1e6 / sets_pf) if sets_pf else float('nan')))
if med('busy') == med('busy'):
    # (busy/idle are NaN for arms whose binary predates the replay-thread telemetry)
    emit('replay-thread load: busy %.2f ms/frame, idle %.2f ms/frame (medians of separately published '
         'windows; their sum is not one window\'s period, so it must not be read as 1000/period)'
         % (med('busy'), med('idle')))

# Auxiliary once-per-window lines, restricted to the heavy stretch.
def in_block(regex, keep=0):
    hit = [l for i, l in enumerate(lines) if first_line <= i <= last_line and regex.search(l)]
    return hit[keep:] if keep else hit

# Did the drive actually reach and hold the fight? The known failure mode is the level-load freeze after the
# movie chain: the picture stays a black UI frame and the drive samples it as "dark-ui (mean 0.6, motion 0.1)"
# for the rest of the run. Its windows are heavy too (upload ~60 MB, fps ~20), so fps alone cannot tell them
# apart from the fight (11-13 fps): check the driver's own scene classifications.
if drive_log:
    try:
        dl = open(drive_log, errors='replace').read().splitlines()
    except FileNotFoundError:
        dl = []
    decisions = [l for l in dl if re.search(r'(dark-ui|scene|bar hub|BATTLE|dark-scene)', l)]
    tail = decisions[-30:]
    frozen = sum(1 for l in tail if re.search(r'dark-ui \(mean 0\.[0-7],.*motion 0\.[0-9]\)', l))
    batt = sum(1 for l in dl if 'BATTLE' in l)
    if frozen >= 20:
        emit('battle: FAIL level-load freeze, not the fight: %d of the last %d drive samples are '
             '"dark-ui (mean 0.6, motion ~0.1)" (the movie-chain exit never loaded the level; %d BATTLE '
             'decisions in the run)' % (frozen, len(tail), batt))
        finish(1)

# Current logs identify the present window and already report per-present
# values. Older binaries always divided by 60; rescale those lines according
# to the actual 60- or 300-present block, rather than assuming every block is
# 300 presents. Retirement can shift write-backs between adjacent windows.
walls = []
for i, line in enumerate(lines):
    if not first_line <= i <= last_line:
        continue
    m = wall_re.search(line)
    if m:
        scale = 1.0 if re.search(r'window=\d+ presents', line) else 60.0 / span_presents(i)
        walls.append((m, scale))
if walls:
    wms = [float(m.group(1)) * scale for m, scale in walls]
    last, scale = walls[-1]
    emit('write-back wall: %.2f ms/frame (min %.2f max %.2f, n=%d), '
         'ranges/batch %s, %.1f MB/frame merged'
         % (statistics.median(wms), min(wms), max(wms), len(wms),
            last.group(2), float(last.group(3)) * scale))
else:
    emit('write-back wall: none in the heavy stretch')

subs = [sub_re.search(l) for l in in_block(sub_re)]
if subs:
    emit('submit timing: period %.2f ms  guest-thread cpu %.2f ms  call %.2f ms (median of %d windows)'
         % (statistics.median(float(m.group(2)) for m in subs),
            statistics.median(float(m.group(3)) for m in subs),
            statistics.median(float(m.group(4)) for m in subs), len(subs)))
for label, regex in (('compute ranges', comp_re), ('write-backs', wb_re), ('pipelined', pipe_re),
                     ('write-back chunks', chunk_re), ('write-back snapshots', snap_re)):
    hits = in_block(regex)
    if hits:
        emit('%s: %s' % (label, hits[-1].split(': ', 1)[1] if ': ' in hits[-1] else hits[-1]))
# Per-frame restatement of the two counters the audit needs, with their real 300-present cadence.
comp = [l for i, l in enumerate(lines) if first_line <= i <= last_line and comp_re.search(l)]
if comp:
    m = re.search(r'(\d+) bindings of written copies, (\d+) merged, (\d+) reads composed', comp[-1])
    if m:
        n = span_presents(comp[-1])
        emit('compute ranges per frame (counters span %d presents): %.0f written-copy bindings, '
             '%.0f merged, %.0f composed reads'
             % (n, float(m.group(1)) / n, float(m.group(2)) / n, float(m.group(3)) / n))
wb = [l for i, l in enumerate(lines) if first_line <= i <= last_line and wb_re.search(l)]
if wb:
    m = re.search(r'(\d+) whole \(([\d.]+) MB, no guest snapshot\), (\d+) merged \(([\d.]+) MB\)', wb[-1])
    if m:
        n = span_presents(wb[-1])
        emit('write-backs per frame (counters span %d presents): %.1f whole (%.2f MB), %.1f merged (%.1f MB)'
             % (n, float(m.group(1)) / n, float(m.group(2)) / n, float(m.group(3)) / n,
                float(m.group(4)) / n))
# NX_REPLAY_COUNTS (the A/B's counter line): verbatim, plus the per-frame composed-read rate.
rc = [l for i, l in enumerate(lines) if first_line <= i <= last_line and 'replay counts (NX_REPLAY_COUNTS' in l]
if rc:
    emit('replay counts (verbatim): %s' % rc[-1].split('nvn: ', 1)[-1])
    m = re.search(r'\): (\d+) composed reads \(([\d.]+) MB/frame, (\d+) under 64 KiB\); why: (\d+) cross/meet, '
                  r'(\d+) past end, (\d+) past cap, (\d+) misaligned; size: (\d+) <256 B, (\d+) <1 K, (\d+) <4 K, '
                  r'(\d+) <16 K, (\d+) <64 K, (\d+) up; keys: (\d+) distinct \((\d+) repeats\); '
                  r'(\d+) answered from the copy cache; (\d+) copy_regions calls, (\d+) vkCmdCopyBuffer, '
                  r'(\d+) transfer barriers', rc[-1])
    if m:
        # groups: 1 composed, 2 MB/frame (float), 3 under64K, 4-7 why, 8-13 size, 14 distinct,
        # 15 repeats, 16 cache hits, 17 copy_regions calls, 18 vkCmdCopyBuffer, 19 barriers
        g = [int(x) for x in m.groups()[:1] + m.groups()[2:]]
        composed, distinct, repeats, cache, copies, barriers = g[0], g[11], g[12], g[13], g[15], g[16]
        n = span_presents(rc[-1])
        emit('replay counts per frame (counters span %d presents): %.1f composed reads, %.1f answered from the copy cache, '
             '%.1f vkCmdCopyBuffer, %.1f transfer barriers, %.1f distinct keys (repeat rate %.1f%%)'
             % (n, composed / n, cache / n, copies / n, barriers / n, distinct / n,
                100.0 * repeats / composed if composed else 0.0))

buses = [busy_re.search(l) for l in in_block(busy_re)]
if buses:
    emit('gpu busy (NX_GPU_PROFILE=3): busy %.2f ms/frame  parts %.1f/frame  span %.2f ms/frame (median of %d windows)'
         % (statistics.median(float(m.group(1)) for m in buses),
            statistics.median(float(m.group(2)) for m in buses),
            statistics.median(float(m.group(3)) for m in buses), len(buses)))

# Per-pass profile (NX_GPU_PROFILE=1) of the last profiled window in the heavy stretch.
prof = [l for i, l in enumerate(lines) if first_line <= i <= last_line and l.startswith('gpu profile:')]
if prof:
    emit('gpu per-pass profile (NX_GPU_PROFILE=1, last window in the stretch):')
    for l in prof[-28:]:
        emit('  ' + l)

# The same numbers for both thresholds, whole run and largest cluster, so the filter cannot hide a
# change: strict first (the round-to-round comparison), then the wide fight-like set.
def stat_line(label, keys, cl):
    if not keys:
        emit('%s: none' % label)
        return
    fs = sorted(rows[k]['fps'] for k in keys)
    p10 = fs[min(len(fs) - 1, int(round(0.10 * (len(fs) - 1))))]
    emit('%s: %d windows; fps median %.1f p10 %.1f min %.1f; replay %.2f fence %.2f upload %.2f MB'
         % (label, len(keys), statistics.median(fs), p10, min(fs),
            statistics.median(rows[k]['replay'] for k in keys),
            statistics.median(rows[k]['fence'] for k in keys),
            statistics.median(rows[k]['upload'] for k in keys)))
    if cl:
        big = max(cl, key=len)
        fsl = sorted(rows[k]['fps'] for k in big)
        emit('%s, largest cluster: %d windows; fps median %.1f min %.1f; replay %.2f fence %.2f upload %.2f MB'
             % (label, len(big), statistics.median(fsl), min(fsl),
                statistics.median(rows[k]['replay'] for k in big),
                statistics.median(rows[k]['fence'] for k in big),
                statistics.median(rows[k]['upload'] for k in big)))
    # The workload-normalized cost of the same set: the number to judge a CPU cut by when the fps
    # spread between arms is scheduling (replay-thread idle), not work.
    wl = [rows[k] for k in keys if rows[k]['draws'] is not None]
    if wl:
        dpf = statistics.median(r['draws'] for r in wl)   # already per frame for its own block
        sets_wl = [r for r in wl if r['sets'] is not None]
        spf = statistics.median(r['sets'] for r in sets_wl) if sets_wl else float('nan')
        rep = statistics.median(r['replay'] for r in wl)
        emit('%s, cost: replay %.2f ms / %.0f replayed draws per frame = %.0f ns/draw (%.1f us); '
             '%.0f sets/frame = %.1f ns/set'
             % (label, rep, dpf, rep * 1e6 / dpf if dpf else float('nan'), (rep * 1e6 / dpf / 1000.0) if dpf else float('nan'),
                spf, rep * 1e6 / spf if spf else float('nan')))
stat_line('strict (upload > %.0f MB)' % STRICT_UPLOAD, strict_keys, strict_clusters)
stat_line('wide (upload > %.0f MB)' % WIDE_UPLOAD, wide_keys, wide_clusters)
stat_line('heavy (upload > %.0f MB)' % HEAVY_UPLOAD, heavy_keys, heavy_clusters)

# Raw window list for like-for-like comparison: every wide battle window, * when in the reported
# stretch, S when it also passes the strict threshold.
if out_prefix:
    with open(out_prefix + '-windows.txt', 'w') as f:
        for k in wide_keys:
            f.write('%s%swindow %d (frames %d-%d, log line %d): %s\n'
                    % ('*' if k in block else ' ', 'S' if k in strict_keys else '-', k, 60 * k + 1,
                       60 * (k + 1), rows[k]['line'] + 1,
                       lines[rows[k]['line']].split('nvn/vk: ', 1)[-1]))
    emit('raw window list: %s-windows.txt (* = in the reported stretch, S = strict threshold)' % out_prefix)

# When did the fight start/end in drive-log wall time? The game log has no timestamps, but the drive
# log prints the frame number it sampled, so a window index -> frame -> time map exists.
if drive_log:
    try:
        f2t = {}
        for l in open(drive_log, errors='replace'):
            m = re.match(r'\[(\d\d:\d\d:\d\d)\] (\d+)\.ppm', l)
            if m:
                f2t[int(m.group(2))] = m.group(1)
        if f2t:
            def t_of(frame):
                ks = [k for k in f2t if k <= frame]
                return f2t[max(ks)] if ks else '?'
            emit('fight span in drive time: %s -> %s (frames %d-%d of %d)'
                 % (t_of(60 * block[0] + 1), t_of(60 * (block[-1] + 1)), 60 * block[0] + 1,
                    60 * (block[-1] + 1), 60 * len(rows)))
    except FileNotFoundError:
        pass

# Evidence that the drive actually walked into the fight: the driver's own BATTLE decisions.
if drive_log:
    try:
        dl = open(drive_log, errors='replace').read().splitlines()
        battles = [l for l in dl if 'BATTLE' in l]
        skips = [l for l in dl if 'skip it' in l]
        emit('drive: %d BATTLE decisions, last "%s"; %d movie skips: %s'
             % (len(battles), battles[-1] if battles else 'none', len(skips),
                ' | '.join(s.split('] ', 1)[-1] for s in skips) or 'none'))
    except FileNotFoundError:
        emit('drive: no drive log at %s' % drive_log)

# State identity independent of fps/upload, calibrated per scene with BATTLE_MIN_SETS: the Prologue fight
# peaks at 2,349-2,982 sets per submit while its movie/menu states stay at 23-941; the Chapter 2 fight peaks
# at 1,819-1,894 while the same run's movie/menu states stay at 24-225. A run whose reported stretch never
# reaches the scene's threshold is not the fight, whatever its windows say.
pools = [int(pool_re.search(l).group(1)) for i, l in enumerate(lines)
         if first_line <= i <= last_line and pool_re.search(l)]
if pools:
    emit('descriptor pool peak per submit in the stretch: median %d, max %d sets (fight threshold %d)'
         % (statistics.median(pools), max(pools), min_sets))
    if max(pools) < min_sets:
        emit('battle: FAIL descriptor pool peak max %d < %d: this stretch is not the fight state'
             % (max(pools), min_sets))
        finish(1)
else:
    emit('battle: FAIL no descriptor pool evidence in the reported stretch; fight state unverified')
    finish(1)

fatal = sum(1 for l in lines if 'FATAL' in l)
crash = sum(1 for l in lines if 'CRASH:' in l)
if fatal or crash:
    emit('battle: FAIL fatal: %d  crash: %d' % (fatal, crash))
    finish(1)

# Independent "is the GPU idle" check: the amdgpu busy flag, sampled from outside the game. The value
# is 0 or 99 per sample, so the mean is the fraction of sampled time the GPU was busy. Attribution is
# by time: the heavy stretch is the last `heavy span` seconds before the game quit (the fight runs to
# the end), the earlier samples are the movie/menu phase.
if gpu_csv:
    try:
        samples, clocks = [], []
        for row in open(gpu_csv):
            p = row.split()
            if len(p) >= 2 and p[0].isdigit() and p[1].isdigit():
                samples.append((int(p[0]), int(p[1])))
                if len(p) >= 3 and p[2].isdigit():
                    clocks.append((int(p[0]), int(p[2])))
    except FileNotFoundError:
        samples = []
    if len(samples) > 30:
        vals = [v for _, v in samples]
        end = samples[-1][0]
        heavy_from = end - int(wall)
        heavy_vals = [v for t, v in samples if t >= heavy_from]
        light_vals = [v for t, v in samples if t < heavy_from]
        def pct(v):
            return 100.0 * sum(1 for x in v if x > 50) / len(v) if v else 0.0
        emit('gpu busy%% (amdgpu, outside the game): whole run %.0f%% of %d samples; movie/menu phase %.0f%% '
             '(n=%d); last %.0f s (battle stretch) %.0f%% (n=%d)'
             % (pct(vals), len(vals), pct(light_vals), len(light_vals), wall, pct(heavy_vals), len(heavy_vals)))
        if clocks:
            cvals = [c for t, c in clocks if t >= heavy_from]
            if cvals:
                emit('gpu clock during the battle stretch: median %d MHz (min %d max %d, n=%d)'
                     % (statistics.median(cvals), min(cvals), max(cvals), len(cvals)))
emit('fatal: %d  crash: %d' % (fatal, crash))
emit('battle: OK %d heavy windows' % len(sel))
finish(0)
PY
}

if [ "$extract_only" = "1" ]; then
    extract "$GAME_LOG" "$DRIVE_LOG" "" "$LOGS/$NAME-gpu.csv"
    exit $?
fi

# --- dry run: assemble the experiment plan and launch nothing (never touch the data root) ----------
if b3_dry_run; then
    DATA_ROOT="$DATA_ROOT" "$ROOT/scripts/b3_experiment.sh" "$NAME" "$DRIVE_SECONDS" "$@"
    exit $?
fi

# --- 1. fixed save state ------------------------------------------------------------------------
if [ ! -d "$SAVE_STATE" ]; then
    printf 'error: no pinned save state at %s (back it up once: rsync -a %s/user/ %s)\n' \
        "$SAVE_STATE" "$DATA_ROOT" "$SAVE_STATE" >&2
    exit 2
fi
mkdir -p "$DATA_ROOT"
rm -rf "$DATA_ROOT/user"
cp -a "$SAVE_STATE" "$DATA_ROOT/user"
printf 'battle: restored save state %s -> %s/user\n' "$SAVE_STATE" "$DATA_ROOT"
(cd "$DATA_ROOT/user" && find . -name '*.dat' -exec md5sum {} + | sort)

# --- 2. the run (scripts/b3_experiment.sh takes the machine lock; do not nest) -------------------
# The driver must skip the Prologue's long cutscenes or the drive window goes into the movie chain.
export B3_MOVIE_SKIP_AFTER=${B3_MOVIE_SKIP_AFTER:-20}
GPU_CSV=$LOGS/$NAME-gpu.csv
PROC_TXT=$LOGS/$NAME-proc.txt
GPU_SAMPLER_PID=
PROC_SAMPLER_PID=
cleanup_samplers() {
    local pid
    for pid in "${GPU_SAMPLER_PID:-}" "${PROC_SAMPLER_PID:-}"; do
        [ -n "$pid" ] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    GPU_SAMPLER_PID= PROC_SAMPLER_PID=
}
trap cleanup_samplers EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
GPU_BUSY_FILE=/sys/class/drm/card0/device/gpu_busy_percent
if [ "${GPU_SAMPLE:-1}" = "1" ] && [ -r "$GPU_BUSY_FILE" ]; then
    SCLK_FILE=/sys/class/drm/card0/device/pp_dpm_sclk
    ( while :; do
          sclk=$(sed -n 's/^[0-9]*: \([0-9]*\)Mhz \*.*/\1/p' "$SCLK_FILE" 2>/dev/null)
          printf '%s %s %s\n' "$(date +%s)" "$(cat "$GPU_BUSY_FILE" 2>/dev/null || echo 0)" "${sclk:-}"
          sleep 0.2
      done ) > "$GPU_CSV" 2>/dev/null &
    GPU_SAMPLER_PID=$!
fi
# Optional scheduling/fault evidence for the fight window (BATTLE_SAMPLE=1): wait for the first window that
# looks like the fight (fps < 18 and upload > 55 MB, which the preload/frozen state does not match), then
# snapshot per-thread CPU, minor/major faults, RSS, load average, and 20 s of perf stat counters.
proc_sample() {
    local i pid= window_re n
    window_re='per frame over 60: (0|1[0-7]|[1-9])\.[0-9]+ fps.*upload (5[5-9]|[6-9][0-9])\.[0-9]+ MB'
    for i in $(seq 1 900); do
        pid=$(b3_game_pids "$EXE" | head -1)
        if [ -n "$pid" ]; then
            # two of the last three windows must look like the fight: one stuttering window is not it
            n=$(tail -400 "$GAME_LOG" 2>/dev/null | grep -E "$window_re" | tail -3 | wc -l)
            [ "$n" -ge 2 ] && break
        fi
        sleep 2
    done
    [ -n "$pid" ] || return 0
    {
        printf '=== process sample at %s, pid %s (%s)\n' "$(date +%H:%M:%S)" "$pid" "$(basename "$EXE")"
        grep -E 'Threads|VmRSS|VmHWM' "/proc/$pid/status" 2>/dev/null
        cat /proc/loadavg
        printf 'stat fields (minflt cminflt majflt) before: %s\n' \
            "$(awk '{print $10, $11, $12}' "/proc/$pid/stat" 2>/dev/null)"
        printf 'per-thread %%CPU, 3 samples 3 s apart (tid pcpu state comm):\n'
        for _ in 1 2 3; do
            ps -L -o tid=,pcpu=,stat=,comm= -p "$pid" 2>/dev/null | sort -k2 -nr | head -12
            printf -- '---\n'
            sleep 3
        done
        printf 'stat fields (minflt cminflt majflt) after: %s\n' \
            "$(awk '{print $10, $11, $12}' "/proc/$pid/stat" 2>/dev/null)"
        if command -v perf >/dev/null 2>&1; then
            printf 'perf stat -p %s -- sleep 20 (task-clock, context switches, faults):\n' "$pid"
            perf stat -p "$pid" -- sleep 20 2>&1 | tail -25
        fi
    } > "$PROC_TXT" 2>&1
}
if [ "${BATTLE_SAMPLE:-1}" = "1" ]; then
    proc_sample &
    PROC_SAMPLER_PID=$!
fi
DATA_ROOT="$DATA_ROOT" "$ROOT/scripts/b3_experiment.sh" "$NAME" "$DRIVE_SECONDS" "$@"
rc=$?
cleanup_samplers

# --- 3. heavy-window budget ---------------------------------------------------------------------
printf 'binary: %s  md5 %s\n' "$EXE" "$(md5sum "$EXE" 2>/dev/null | cut -d' ' -f1)"
extract "$GAME_LOG" "$DRIVE_LOG" "$BATTLE_OUT" "$GPU_CSV" || exit 1
[ -s "$PROC_TXT" ] && { printf '\nprocess/scheduling sample (%s):\n' "$PROC_TXT"; sed -n '1,40p' "$PROC_TXT"; }
exit $rc
