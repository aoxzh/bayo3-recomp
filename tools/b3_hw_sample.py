#!/usr/bin/env python3
"""Sample the APU while a game run is in progress, so a low-fps window can be attributed to a cause.

    tools/b3_hw_sample.py --out run.csv [--pid <game pid> | --exe <path>] [--every 0.25]
    tools/b3_hw_sample.py --summarize run.csv [--from <unix time> --to <unix time>]

An FPS number alone cannot separate "the code got slower" from "the 15 W package moved its budget". Each
row holds the GPU clock and busy flag, the package power, the temperature, the mean core clock of the busy
cores, how much of the machine the game used, and how much everything else used. Only read-only sysfs and
procfs nodes are touched; nothing on the system is changed.
"""
import argparse
import csv
import glob
import os
import statistics
import sys
import time


def read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def number(path, default=float('nan')):
    value = read(path)
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def amdgpu_hwmon():
    for name in glob.glob('/sys/class/hwmon/hwmon*/name'):
        if read(name) == 'amdgpu':
            return os.path.dirname(name)
    return None


def cpu_times():
    """Per-CPU busy and total jiffies, by logical CPU number."""
    busy, total = {}, {}
    with open('/proc/stat') as f:
        for line in f:
            if not line.startswith('cpu') or line.startswith('cpu '):
                continue
            parts = line.split()
            values = [int(v) for v in parts[1:9]]
            cpu = int(parts[0][3:])
            idle = values[3] + values[4]
            total[cpu] = sum(values)
            busy[cpu] = total[cpu] - idle
    return busy, total


def process_jiffies(pid):
    if not pid:
        return 0
    data = read(f'/proc/{pid}/stat')
    if not data:
        return 0
    fields = data[data.rfind(')') + 2:].split()
    return int(fields[11]) + int(fields[12])


def thread_table(pid):
    """(name, jiffies, last cpu) of each thread of the game."""
    rows = {}
    if not pid:
        return rows
    for task in glob.glob(f'/proc/{pid}/task/*'):
        data = read(task + '/stat')
        if not data:
            continue
        name = data[data.find('(') + 1:data.rfind(')')]
        fields = data[data.rfind(')') + 2:].split()
        rows[os.path.basename(task)] = (name, int(fields[11]) + int(fields[12]), int(fields[36]))
    return rows


def find_pid(exe):
    want = os.path.realpath(exe)
    for proc in glob.glob('/proc/[0-9]*'):
        try:
            real = os.readlink(proc + '/exe').replace(' (deleted)', '')
        except OSError:
            continue
        if real == want:
            return int(os.path.basename(proc))
    return None


def sample(args):
    hwmon = amdgpu_hwmon()
    device = '/sys/class/drm/card0/device'
    hz = os.sysconf('SC_CLK_TCK')
    cpus = sorted(cpu_times()[0])
    freq_paths = {c: f'/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq' for c in cpus}
    fields = ['time', 'sclk_mhz', 'gpu_busy', 'power_w', 'temp_c', 'cpu_mhz_busy', 'cpu_mhz_max',
              'cpu_used', 'game_used', 'other_used', 'replay_used', 'replay_cpu', 'sibling_used',
              'major_faults']
    pid = args.pid
    with open(args.out, 'w', newline='') as out:
        writer = csv.writer(out)
        writer.writerow(fields)
        prev_busy, prev_total = cpu_times()
        prev_game = process_jiffies(pid)
        prev_threads = thread_table(pid)
        prev_t = time.monotonic()
        prev_major = 0
        while True:
            time.sleep(args.every)
            if not pid and args.exe:
                pid = find_pid(args.exe)
                prev_game = process_jiffies(pid)
                prev_threads = thread_table(pid)
            elif pid and not os.path.exists(f'/proc/{pid}'):
                if args.exe:
                    break
                break
            now = time.monotonic()
            dt = max(now - prev_t, 1e-3)
            busy, total = cpu_times()
            used = {c: (busy[c] - prev_busy[c]) / max(total[c] - prev_total[c], 1) for c in busy}
            game = process_jiffies(pid)
            game_cores = (game - prev_game) / hz / dt
            threads = thread_table(pid)
            replay_used, replay_cpu = 0.0, -1
            for tid, (name, jiffies, last) in threads.items():
                if name == 'nvn replay':
                    replay_used = (jiffies - prev_threads.get(tid, (name, jiffies, last))[1]) / hz / dt
                    replay_cpu = last
            sibling = float('nan')
            if replay_cpu >= 0:
                text = read(f'/sys/devices/system/cpu/cpu{replay_cpu}/topology/thread_siblings_list', '')
                ids = [int(x) for x in text.replace('-', ',').split(',') if x.isdigit()]
                others = [c for c in ids if c != replay_cpu]
                if others:
                    sibling = used.get(others[0], float('nan'))
            freqs = {c: number(freq_paths[c]) / 1000 for c in cpus}
            active = [freqs[c] for c in cpus if used[c] > 0.5]
            major = 0
            if pid:
                data = read(f'/proc/{pid}/stat')
                if data:
                    major = int(data[data.rfind(')') + 2:].split()[9])
            cpu_used = sum(used.values())
            sclk = number(hwmon + '/freq1_input') / 1e6 if hwmon else float('nan')
            power = number(hwmon + '/power1_input') / 1e6 if hwmon else float('nan')
            if power != power and hwmon:
                power = number(hwmon + '/power1_average') / 1e6
            temp = number(hwmon + '/temp1_input') / 1000 if hwmon else float('nan')
            writer.writerow([f'{time.time():.3f}', f'{sclk:.0f}', read(device + '/gpu_busy_percent', ''),
                             f'{power:.2f}', f'{temp:.1f}',
                             f'{statistics.mean(active):.0f}' if active else '',
                             f'{max(freqs.values()):.0f}', f'{cpu_used:.2f}', f'{game_cores:.2f}',
                             f'{max(cpu_used - game_cores, 0):.2f}', f'{replay_used:.2f}', replay_cpu,
                             f'{sibling:.2f}', major - prev_major if prev_major else 0])
            out.flush()
            prev_busy, prev_total, prev_game, prev_threads, prev_t, prev_major = busy, total, game, threads, now, major


def summarize(args):
    rows = list(csv.DictReader(open(args.summarize)))
    if args.start is not None or args.stop is not None:
        rows = [r for r in rows if (args.start is None or float(r['time']) >= args.start) and
                (args.stop is None or float(r['time']) <= args.stop)]
    if not rows:
        sys.exit('no samples in range')

    def stat(name):
        values = []
        for row in rows:
            try:
                value = float(row[name])
            except (TypeError, ValueError):
                continue
            if value == value:
                values.append(value)
        if not values:
            return 'n/a'
        values.sort()
        return (f'median {statistics.median(values):.2f}  p10 {values[len(values) // 10]:.2f}  '
                f'p90 {values[len(values) * 9 // 10]:.2f}')

    print(f'{len(rows)} samples')
    for name in ('sclk_mhz', 'gpu_busy', 'power_w', 'temp_c', 'cpu_mhz_busy', 'cpu_used', 'game_used',
                 'other_used', 'replay_used', 'sibling_used', 'major_faults'):
        print(f'{name:14s} {stat(name)}')
    clocks = [float(r['sclk_mhz']) for r in rows if r['sclk_mhz'] not in ('', 'nan')]
    if clocks:
        print(f'sclk <= 400 MHz in {100 * sum(1 for c in clocks if c <= 400) / len(clocks):.0f}% of samples')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out')
    parser.add_argument('--pid', type=int)
    parser.add_argument('--exe')
    parser.add_argument('--every', type=float, default=0.25)
    parser.add_argument('--summarize')
    parser.add_argument('--from', dest='start', type=float)
    parser.add_argument('--to', dest='stop', type=float)
    args = parser.parse_args()
    if args.summarize:
        summarize(args)
    elif args.out:
        sample(args)
    else:
        parser.error('--out or --summarize is required')


if __name__ == '__main__':
    main()
