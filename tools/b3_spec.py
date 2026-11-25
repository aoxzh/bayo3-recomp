#!/usr/bin/env python3
"""Whether this computer can run the game: memory, CPU and GPU against a minimum (the launcher asks before Play and
setup below it) and a recommended level (a warning only). The phone's rules are in b3_phone.spec_rules and the app's
DeviceCheck.java.

Measured: an Intel UHD iGPU ran the Flower Kingdom intro at 26-31 fps docked; the Steam Deck (4 Zen 2 cores, RDNA2
8 CU, 16 GB) holds 50-60 fps at 720p; a Core Ultra / Arc iGPU with 32 GB plays at 60 and above. The game's recompiled
code is CPU-bound (one guest thread and the replay thread), and the build itself wants memory: a module recompile
peaks near 1 GB each, the shader translation and the C++ compile several GB, beside the game's own ~5 GB.

Each rule returns 'ok', 'weak' (may struggle: a warning) or 'below' (under the minimum), or None for a part it does
not know: unknown parts are never refused."""
import os
from pathlib import Path
import re
import subprocess
import sys

MINIMUM = ('Minimum: 12 GB of memory, a CPU like an 8th-gen Core i5 or Ryzen 5 3600, and a Vulkan 1.3 GPU like a '
           'GTX 1050, RX 560 or Intel UHD. Recommended: 16 GB, an 11th-gen Core i5 / Ryzen 5 5600 or newer, and a '
           'GTX 1060 / RX 570 / Iris Xe / Radeon 680M or newer (the Steam Deck is fine).')


def cpu_rule(name, threads=0):
    """('ok' | 'weak' | 'below' | None, why)."""
    n = name or ''
    if threads and threads < 4:
        return 'below', f'{threads} CPU threads (4 at least)'
    if re.search(r'Pentium|Celeron|Atom|Core\(TM\)2|Core\(TM\) m\d|\bN\d{2,3}\b|Athlon|\bFX(\(tm\))?-|\bA\d{1,2}-\d{4}', n):
        return 'below', 'an entry-level CPU'
    if re.search(r'Core\(TM\) Ultra|Core Ultra|Core\(TM\) [3579] \d{3}|Apple M\d|Ryzen AI|Ryzen Z\d|Custom APU 0(405|932)', n):
        return 'ok', ''
    m = re.search(r'\bi([3579])-(\d{4,5})', n)
    if m:
        tier, digits = int(m.group(1)), m.group(2)
        gen = int(digits[:2]) if len(digits) == 5 or digits[0] == '1' else int(digits[0])  # i5-1135G7: 11, i5-8250U: 8
        if tier == 3:
            return ('ok', '') if gen >= 12 else ('weak', 'a Core i3') if gen >= 10 else ('below', 'an older Core i3')
        if gen >= 11:
            return 'ok', ''
        return ('weak', f'a {gen}th-gen Core i{tier}') if gen >= 8 else ('below', f'a {gen}th-gen Core i{tier}')
    m = re.search(r'Ryzen (\d)(?: PRO)? (\d)(\d{3})', n)
    if m:
        tier, series = int(m.group(1)), int(m.group(2))
        if series >= 5 and tier >= 5:
            return 'ok', ''
        if tier >= 7 and series >= 3:
            return 'ok', ''
        if series >= 3:
            return 'weak', f'a Ryzen {tier} {series}000-series'
        return 'below', f'a Ryzen {tier} {series}000-series'
    if threads and threads < 8:
        return 'weak', f'{threads} CPU threads'
    return None, ''


def gpu_rule(name, windows=True):
    """('ok' | 'weak' | 'below' | None, why) for one GPU's name (Windows' adapter name, or Vulkan's)."""
    n = name or ''
    if re.search(r'Microsoft Basic|llvmpipe|SwiftShader|Virtual|VMware|Parallels', n, re.I):
        return 'below', 'no real GPU driver (a software renderer)'
    if re.search(r'Custom GPU 0(405|932)|VANGOGH|Van Gogh|Sephiroth', n, re.I):
        return 'ok', ''  # Steam Deck
    m = re.search(r'RTX|GTX 16\d\d|Titan', n)
    if m:
        return 'ok', ''
    m = re.search(r'GTX (\d{3,4})', n)
    if m:
        v = int(m.group(1))
        if v >= 1060 or v in (970, 980):
            return 'ok', ''
        if v >= 950 or v == 750:  # Maxwell and the GTX 1050: Vulkan 1.3, slow
            return 'weak', f'a {name.strip()}'
        return 'below', f'a {name.strip()} (no Vulkan 1.3 driver)'  # Kepler and older
    if re.search(r'\bGT \d{3,4}|\bMX\s?\d{3}|Quadro K|NVS', n):
        m = re.search(r'MX\s?(\d{3})', n)
        if m and int(m.group(1)) >= 350:
            return 'weak', f'a {name.strip()}'
        return 'below', f'an entry-level {name.strip()}'
    m = re.search(r'RX\s?(\d{3,4})', n)
    if m:
        v = int(m.group(1))
        if v >= 5000 or v in (470, 480, 570, 580, 590) or 'Vega' in n:
            return 'ok', ''
        return 'weak', f'a {name.strip()}'
    if re.search(r'Radeon(\(TM\))? (6[0-9]0M|7[0-9]0M|8[0-9]0M)|Radeon Pro W[5-7]|Vega (56|64)|Radeon VII', n):
        return 'ok', ''
    if re.search(r'Radeon(\(TM\))? (R[579]|HD)\b|FirePro', n):
        # GCN 1-3: AMD's Windows driver stops at Vulkan 1.2; Mesa RADV gives them 1.3 on Linux.
        return ('below', f'a {name.strip()} (no Vulkan 1.3 driver on Windows)') if windows else ('weak', f'a {name.strip()}')
    if re.search(r'Radeon(\(TM\))? (Vega|Graphics)|Radeon(\(TM\))? \d{3}M?\b', n):
        return 'weak', f'integrated {name.strip()}'
    if re.search(r'Arc|Iris\(R\) Xe|Iris Xe', n):
        return 'ok', ''
    if re.search(r'UHD Graphics|Iris|HD Graphics [56]\d\d', n):
        return 'weak', f'integrated {name.strip()}'
    if re.search(r'HD Graphics', n):
        return 'below', f'an older {name.strip()} (no Vulkan 1.3 driver)'
    return None, ''


def pc_rules(memory_gb, cpu, threads, gpus, windows=True):
    """(below, weak): reasons under the minimum and reasons it may struggle. memory_gb as the OS reports it (a
    12 GB machine shows about 11.7), gpus every adapter's name: the best one counts."""
    below, weak = [], []
    if memory_gb and memory_gb < 11:
        below.append(f'{memory_gb:.0f} GB of memory (12 GB at least)')
    elif memory_gb and memory_gb < 15:
        weak.append(f'{memory_gb:.0f} GB of memory (16 GB recommended)')
    verdict, why = cpu_rule(cpu, threads)
    if verdict == 'below':
        below.append(f'{cpu or "the CPU"}: {why}')
    elif verdict == 'weak':
        weak.append(f'{cpu}: {why}, expect a lower frame rate in fights')
    rank = {'ok': 3, 'weak': 2, None: 1, 'below': 0}
    judged = [(gpu_rule(g, windows), g) for g in gpus if g and g.strip()]
    if judged:
        (verdict, why), _ = max(judged, key=lambda j: rank[j[0][0]])
        if verdict == 'below':
            below.append(f'GPU: {why}')
        elif verdict == 'weak':
            weak.append(f'GPU: {why}, expect a lower frame rate')
    return below, weak


def cpu_name():
    if sys.platform == 'win32':
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as k:
                return str(winreg.QueryValueEx(k, 'ProcessorNameString')[0]).strip()
        except OSError:
            return ''
    try:
        for line in Path('/proc/cpuinfo').read_text(errors='replace').splitlines():
            if line.startswith('model name'):
                return line.split(':', 1)[1].strip()
    except OSError:
        pass
    return ''


def gpu_names(root=None):
    """Every display adapter's name: Windows' list, else lspci; plus the Vulkan device a past run logged."""
    names = []
    try:
        if sys.platform == 'win32':
            out = subprocess.run(['powershell', '-NoProfile', '-Command',
                                  '(Get-CimInstance Win32_VideoController).Name'], capture_output=True, text=True,
                                 timeout=20, creationflags=0x08000000).stdout
            names += [l.strip() for l in out.splitlines() if l.strip()]
        else:
            out = subprocess.run(['lspci'], capture_output=True, text=True, timeout=10).stdout
            names += [l.split(': ', 1)[1] for l in out.splitlines() if re.search(r'VGA|3D|Display', l) and ': ' in l]
    except (OSError, subprocess.SubprocessError):
        pass
    if root:  # the game's own log names the Vulkan device it used: the surest name there is
        for log in [Path(root) / ('private/win/logs/game.log' if sys.platform == 'win32' else 'private/b3.log')]:
            try:
                m = re.search(r'nvn/vk: using GPU \d+, ([^(\n]+)', log.read_text(errors='replace'))
                if m:
                    names.append(m.group(1).strip())
            except OSError:
                pass
    return names


def memory_gb():
    try:
        with open('/proc/meminfo') as f:
            return int(next(l for l in f if l.startswith('MemTotal:')).split()[1]) / (1 << 20)
    except (OSError, StopIteration, ValueError):
        pass
    if sys.platform == 'win32':
        import ctypes

        class Status(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                        ('rest', ctypes.c_ulonglong * 6)]
        s = Status()
        s.length = ctypes.sizeof(Status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)):
            return s.total / (1 << 30)
    return 0


def check(root=None):
    """(below, weak, summary) for this computer."""
    cpu, gpus, mem = cpu_name(), gpu_names(root), memory_gb()
    below, weak = pc_rules(mem, cpu, os.cpu_count() or 0, gpus, windows=sys.platform == 'win32')
    summary = f'{cpu or "CPU ?"} | {mem:.0f} GB | ' + (', '.join(dict.fromkeys(gpus)) or 'GPU ?')
    return below, weak, summary


if __name__ == '__main__':
    b, w, s = check(Path(__file__).resolve().parents[1])
    print(s)
    for x in b:
        print('below the minimum:', x)
    for x in w:
        print('may struggle:', x)
    print('ok' if not b and not w else 'below the minimum' if b else 'may struggle')
