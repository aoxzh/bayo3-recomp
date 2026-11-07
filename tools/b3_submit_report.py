#!/usr/bin/env python3
"""Join submit diagnostics with workload counters and sampled scene labels.

No upload-volume predicate is used. FPS uses equal-frame window durations.
Draw/dispatch rates are differences of cumulative production counters.
Scene labels are sampled screenshots, not proof of every intermediate frame.
"""
import argparse
import json
from pathlib import Path
import re
import statistics


def trace_jobs(game):
    events = []
    for line in game.read_text(errors='replace').splitlines():
        m = re.search(r'submit-trace: ns (\d+) tid (\d+) frame (\d+) (\S+) (\d+)', line)
        if m:
            ns, tid, frame, event, ident = m.groups()
            events.append((int(ns), int(tid), int(frame), event, int(ident)))
    jobs = []
    for start, tid, frame, event, ident in events:
        if event != 'worker-begin':
            continue
        stop = next((e[0] for e in events if e[3] == 'worker-done' and e[4] == ident and e[1] == tid), None)
        if stop is None:
            continue  # Trace boundaries can omit one endpoint.
        subset = [e for e in events if start <= e[0] <= stop and e[1] == tid]
        job = {'id': ident, 'guest_frame_at_start': frame, 'start_ns': start, 'worker_ms': (stop - start) / 1e6}
        for a, b, label in [('queue-submit-begin', 'queue-submit-end', 'queue_ms'),
                            ('gpu-wait-begin', 'gpu-wait-return', 'wait_ms'),
                            ('writeback-begin', 'writeback-end', 'writeback_ms'),
                            ('replay-return', 'guest-fences-published', 'tail_wall_ms')]:
            pairs = []
            for e in subset:
                if e[3] != a:
                    continue
                end = next((f for f in subset if f[0] >= e[0] and f[3] == b and f[4] == e[4]), None)
                if end:
                    pairs.append((end[0] - e[0]) / 1e6)
            if pairs:  # Missing publication must not masquerade as a zero-cost tail.
                job[label] = sum(pairs)
            if label == 'queue_ms':
                job['queue_calls'] = len(pairs)
        enqueue = next((e[0] for e in events if e[3] == 'guest-enqueue' and e[4] == ident), None)
        if enqueue is not None:
            job['enqueue_to_worker_ms'] = (start - enqueue) / 1e6
        jobs.append(job)
    return jobs


def parse(game, drive):
    labels = []
    for line in drive.read_text(errors='replace').splitlines():
        m = re.search(r'(\d+)\.ppm: ([\w-]+) \(mean', line)
        if m:
            labels.append((int(m[1]), m[2]))
    rows, workloads, completion_windows = [], [], []
    pending_draws = pending_dispatches = None
    previous = (0, 0, 0)
    for line in game.read_text(errors='replace').splitlines():
        m = re.search(r'guest-fence completion over 600 presents: (\d+) calls ([\d.]+) ms total', line)
        if m:
            calls, total_ms = int(m[1]), float(m[2])
            completion_windows.append({'report_index': len(completion_windows) + 1,
                                       'presents': 600, 'calls': calls, 'total_ms': total_ms,
                                       'ms_per_present': total_ms / 600})
        m = re.search(r'nvn draws: (\d+) total', line)
        if m:
            pending_draws = int(m[1])
        m = re.search(r'nvn passes: .*?, (\d+) dispatches', line)
        if m:
            pending_dispatches = int(m[1])
        m = re.search(r'per frame over 60: ([\d.]+) fps, ([\d.]+) submits, replay ([\d.]+) ms, submit \(encode\) ([\d.]+) ms, fence wait ([\d.]+) ms, other ([\d.]+) ms, present ([\d.]+) ms, upload ([\d.]+) MB', line)
        if m:
            row = dict(zip(('fps', 'submits', 'replay', 'encode', 'fence', 'other', 'present', 'upload'), map(float, m.groups())))
            row['frame_end'] = (len(rows) + 1) * 60
            rows.append(row)
            if pending_draws is not None and pending_dispatches is not None:
                end = row['frame_end']
                start, old_draws, old_dispatches = previous
                workloads.append({'start': start, 'end': end,
                                  'draws': (pending_draws - old_draws) / (end - start),
                                  'dispatches': (pending_dispatches - old_dispatches) / (end - start)})
                previous = end, pending_draws, pending_dispatches
                pending_draws = pending_dispatches = None
        if not rows:
            continue
        row = rows[-1]
        m = re.search(r'imports over 60 frames: attempts (\d+) hits (\d+) made (\d+) lookup ([\d.]+).*cached objects (\d+) allocated ([\d.]+) MiB unique ([\d.]+)', line)
        if m:
            row.update(zip(('import_attempts', 'import_hits', 'import_made', 'import_lookup', 'import_objects', 'import_allocated_mib', 'import_unique_mib'), map(float, m.groups())))
        m = re.search(r'async API over 60 frames: calls (\d+) end ([\d.]+) reset ([\d.]+) queue ([\d.]+).*queue-max ([\d.]+).*all waits (\d+) ([\d.]+) writeback ([\d.]+) (?:drain-)?janitor-join ([\d.]+).*embedded-tail (\d+) ([\d.]+)', line)
        if m:
            row.update(zip(('queue_calls', 'end_cb', 'reset_fence', 'queue', 'queue_max', 'wait_calls', 'all_wait', 'writeback', 'drain_join', 'tail_calls', 'tail'), map(float, m.groups())))
        m = re.search(r'reuse-janitor-join ([\d.]+) ms/frame', line)
        if m:
            row['reuse_join'] = float(m[1])
        m = re.search(r'gpu busy: ([\d.]+) ms/frame \(([\d.]+) parts/frame\), span ([\d.]+) ms/frame, gap ([\d.]+)', line)
        if m:
            row.update(zip(('gpu_busy', 'gpu_parts', 'gpu_span', 'gpu_gap'), map(float, m.groups())))
        m = re.search(r'watch snapshot active (\d+) overlap objects (\d+) unique ([\d.]+).*add-attempts (\d+) rearm (\d+) remove (\d+) faults (\d+)', line)
        if m:
            row.update(zip(('watch_active', 'watch_objects', 'watch_overlap_mib', 'watch_add', 'watch_rearm', 'watch_remove', 'watch_faults'), map(float, m.groups())))
    summed = {'queue_calls', 'wait_calls', 'tail_calls', 'import_attempts', 'import_hits', 'import_made',
              'watch_add', 'watch_rearm', 'watch_remove', 'watch_faults'}
    peaked = {'queue_max', 'import_objects', 'import_allocated_mib', 'import_unique_mib',
              'watch_active', 'watch_objects', 'watch_overlap_mib'}
    groups = []
    for w in workloads:
        if w['end'] - w['start'] != 300:
            continue
        subset = [r for r in rows if w['start'] < r['frame_end'] <= w['end']]
        sampled = [label for f, label in labels if w['start'] < f <= w['end']]
        if len(subset) != 5 or not all('gpu_busy' in r and 'queue' in r for r in subset):
            continue
        g = dict(w)
        g['labels'] = sampled
        g['sampled_combat'] = len(sampled) >= 2 and all(s == 'combat' for s in sampled)
        g['fps'] = len(subset) / sum(1 / r['fps'] for r in subset)
        for key in subset[0]:
            if key not in ('fps', 'frame_end') and all(key in r for r in subset):
                values = [r[key] for r in subset]
                g[key] = sum(values) if key in summed else max(values) if key in peaked else statistics.mean(values)
        groups.append(g)
    return {'group_summed_fields': sorted(summed), 'group_peak_fields': sorted(peaked),
            'frame_index': 'runtime presents (complete single-run log), not guest g_frame trace IDs',
            'guest_fence_completion_windows': completion_windows,
            'rows': rows, 'groups': groups, 'trace_jobs': trace_jobs(game)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('drive', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    text = json.dumps(parse(args.game, args.drive), indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
