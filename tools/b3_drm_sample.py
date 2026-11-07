#!/usr/bin/env python3
"""Sample per-process DRM accounting; deduplicate FDs sharing a DRM client.

These counters cover the entire DRM client, not just imported guest buffers.
Run concurrently with b3_experiment.sh; SIGINT/SIGTERM ends sampling cleanly.
"""
import argparse
import json
from pathlib import Path
import signal
import time


def snapshot(pid):
    proc = Path('/proc') / str(pid)
    clients = {}
    for fd in (proc / 'fdinfo').iterdir():
        try:
            fields = dict(line.split(':', 1) for line in fd.read_text().splitlines() if ':' in line)
        except (OSError, ValueError):
            continue
        fields = {k: v.strip() for k, v in fields.items()}
        if 'drm-client-id' not in fields:
            continue
        key = (fields.get('drm-pdev', ''), fields['drm-client-id'])
        clients.setdefault(key, {k: v for k, v in fields.items() if k.startswith(('drm-', 'amd-'))})
    status = {}
    for line in (proc / 'status').read_text().splitlines():
        if line.startswith(('VmRSS:', 'VmSwap:', 'VmPeak:')):
            key, value = line.split(':', 1)
            status[key] = value.strip()
    sensors = {}
    for hwmon in Path('/sys/class/hwmon').glob('hwmon*'):
        try:
            name = (hwmon / 'name').read_text().strip()
            if name not in ('amdgpu', 'k10temp', 'acpitz'):
                continue
            for item in hwmon.iterdir():
                if item.name.startswith(('temp', 'power')) and item.name.endswith(('_input', '_average', '_cap')):
                    sensors[name + '/' + item.name] = item.read_text().strip()
        except OSError:
            continue
    for item in Path('/sys/devices/system/cpu').glob('cpu[0-9]*/cpufreq/scaling_cur_freq'):
        try:
            sensors[item.parent.parent.name + '/frequency_khz'] = item.read_text().strip()
        except OSError:
            continue
    return {'pid': pid, 'wall_ns': time.time_ns(), 'monotonic_ns': time.monotonic_ns(),
            'clients': list(clients.values()), 'status': status, 'sensors': sensors}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=1800)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    running = True

    def stop(signum, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    deadline = time.monotonic() + args.seconds
    # Exclusive creation protects previous evidence.
    with args.output.open('x') as out:
        while running and time.monotonic() < deadline:
            for proc in Path('/proc').iterdir():
                if not proc.name.isdigit():
                    continue
                try:
                    if (proc / 'exe').resolve() != exe:
                        continue
                    out.write(json.dumps(snapshot(int(proc.name))) + '\n')
                    out.flush()
                except (OSError, PermissionError):
                    continue
            time.sleep(1)


if __name__ == '__main__':
    main()
