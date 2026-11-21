#!/usr/bin/env python3
"""The game for an Android phone, built on this computer and put on the phone over USB (`b3_win.py phone`).

The phone can build the game itself (the APK's SET UP: 30-60 minutes, warm), but a computer that has already set the
game up has everything but the phone's compiler output: the generated code (private/win/cpu/b3/gen), the shaders and
the analysis images. This compiles that code for arm64 Android with the computer's own clang, against what the release
APK carries for its on-phone setup (assets/ondevice.zip: the NDK sysroot, clang's resource folder, the runtime archive
and its headers; no game code), then:

  1. installs the release APK, unchanged (adb install; an APK never carries game code, so it updates like any other),
  2. copies the game's data to Internal storage/Bayonetta 3 (adb push; only what is missing or changed),
  3. hands the game's library to the app (adb forward to the launcher's local socket, games/.../GameImport.java),
     which keeps it in its private folder, where the on-phone setup would have put it.

  python tools/b3_win.py phone [--apk <release APK>] [--serial <adb serial>] [--build-only] [--jobs N]

The APK must be the release this checkout belongs to (its runtime matches the generated code): VERSION in a release
folder names it and it is downloaded from the project's releases; a checkout from git passes --apk.
"""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
import zipfile

import b3_win
from b3_win import ROOT, WIN, SetupError, say

REPO = 'aoxzh/bayo3-recomp'
PACKAGE = 'io.github.bayonetta3recomp'
PHONE = WIN / 'phone'
REMOTE = '/sdcard/Bayonetta 3'
MODULES = ('main', 'subsdk0', 'sdk')
PLATFORM_TOOLS = {'win32': 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip',
                  'linux': 'https://dl.google.com/android/repository/platform-tools-latest-linux.zip',
                  'darwin': 'https://dl.google.com/android/repository/platform-tools-latest-darwin.zip'}


# ---- the release APK and what it carries -------------------------------------------------------------------------

def version():
    v = ROOT / 'VERSION'
    return v.read_text(encoding='utf-8').strip() if v.is_file() else None


def download(url, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + '.part')
    say(f'phone: downloading {url}')
    with urllib.request.urlopen(url, timeout=60) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    os.replace(part, dest)


def release_apk(given):
    if given:
        apk = Path(given).resolve()
        if not apk.is_file():
            raise SetupError(f'{apk}: no such APK')
        return apk
    v = version()
    if not v:
        raise SetupError('this checkout is not a release (no VERSION file): pass --apk <the APK built from it>')
    apk = PHONE / f'Bayonetta3Recomp-{v}-android-arm64.apk'
    if not apk.is_file():
        download(f'https://github.com/{REPO}/releases/download/{v}/{apk.name}', apk)
    return apk


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def apk_toolset(apk):
    """The APK's on-phone toolset (sysroot, clang resources, the runtime archive and headers), unpacked once per
    APK into private/win/phone/toolset-<sha>."""
    digest = sha256(apk)[:16]
    out = PHONE / f'toolset-{digest}'
    if (out / 'complete').is_file():
        return out
    say(f'phone: unpacking the toolset from {apk.name}')
    shutil.rmtree(out, ignore_errors=True)
    staging = out.with_name(out.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    with zipfile.ZipFile(apk) as z, zipfile.ZipFile(io.BytesIO(z.read('assets/ondevice.zip'))) as inner:
        for info in inner.infolist():
            name = info.filename
            if '..' in name.split('/') or name.startswith('/'):
                continue
            if name.startswith('toolset/') or name.startswith('repo/runtime/include/'):
                inner.extract(info, staging)
    for need in ('toolset/sysroot', 'toolset/clang', 'toolset/runtime/libbayonetta3.a', 'repo/runtime/include'):
        if not (staging / need).exists():
            raise SetupError(f'{apk.name} has no {need}: is it the release APK (with the on-phone setup)?')
    (staging / 'complete').write_text(apk.name + '\n', encoding='utf-8')
    os.replace(staging, out)
    return out


# ---- the build ---------------------------------------------------------------------------------------------------

def clang(found):
    """The computer's clang (the release toolset's llvm-mingw on Windows, its LLVM on Linux): it targets arm64
    Android with --target; ld.lld beside it links."""
    cxx = found.get('cxx')
    if not cxx or not Path(cxx).exists():
        raise SetupError('no clang found: run the setup first (python tools/b3_win.py setup)')
    return Path(cxx)


def build(found, toolset, jobs):
    import concurrent.futures
    gen = WIN / 'cpu/b3/gen'
    if not all((gen / m).is_dir() for m in MODULES):
        raise SetupError(f'no generated code in {gen}: set the game up on this computer first')
    out = PHONE / 'build'
    cxx = clang(found)
    target = ['--target=aarch64-linux-android29', f'--sysroot={toolset / "toolset/sysroot"}',
              f'-resource-dir={toolset / "toolset/clang"}']
    common = [cxx, *target, '-std=gnu++20', '-O2', '-DNDEBUG', '-DNX_BAYONETTA3=1', '-fPIC', '-fvisibility=hidden',
              '-fno-semantic-interposition', '-march=armv8.2-a+crypto', '-w',
              '-I', toolset / 'repo/runtime/include']
    # A change of compiler, flags or the runtime's headers compiles everything again; another APK with the same
    # runtime (its toolset unpacked into another folder) does not.
    h = hashlib.sha256(' '.join(str(c).replace(str(toolset), '<toolset>') for c in common).encode())
    for header in sorted((toolset / 'repo/runtime/include').rglob('*')):
        if header.is_file():
            h.update(header.relative_to(toolset).as_posix().encode() + header.read_bytes())
    stamp = h.hexdigest()[:16]
    if (out / 'stamp').is_file() and (out / 'stamp').read_text().strip() != stamp:
        shutil.rmtree(out / 'obj', ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'stamp').write_text(stamp + '\n')
    work = []
    for module in MODULES:
        (out / 'obj' / module).mkdir(parents=True, exist_ok=True)
        for src in sorted((gen / module).glob('*.cpp')):
            work.append((module, src, out / 'obj' / module / (src.stem + '.o')))
    todo = [w for w in work if not (w[2].exists() and w[2].stat().st_mtime >= w[1].stat().st_mtime)]
    jobs = jobs or max(1, (os.cpu_count() or 4) - 1)
    say(f'phone: compiling {len(todo)} of {len(work)} files for arm64 Android ({jobs} at a time)')
    log = WIN / 'logs' / f'phone-{time.strftime("%Y%m%d-%H%M%S")}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    started, done = time.time(), 0

    def one(item):
        module, src, obj = item
        part = obj.with_suffix('.o.part')
        r = subprocess.run([str(c) for c in common + ['-I', gen / module, '-c', src, '-o', part]],
                           capture_output=True, text=True)
        if r.returncode:
            return f'{src.name}: {r.stderr[-2000:]}'
        os.replace(part, obj)
        return None

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for error in pool.map(one, todo):
            if error:
                with open(log, 'a', encoding='utf-8') as f:
                    f.write(error + '\n')
                raise SetupError(f'compile failed (log: {log}): {error[:300]}')
            done += 1
            if done % 25 == 0 or done == len(todo):
                say(f'phone: build: {done}/{len(todo)} files')
    rsp = out / 'objects.rsp'
    rsp.write_text('\n'.join(f'"{o.as_posix()}"' for _m, _s, o in work), encoding='utf-8')
    lib = out / 'libbayonetta3.so'
    say('phone: linking libbayonetta3.so')
    b3_win.run([cxx, *target, '-fuse-ld=lld', '-static-libstdc++', '-shared', '-o', lib,
                '-Wl,--whole-archive', toolset / 'toolset/runtime/libbayonetta3.a', '-Wl,--no-whole-archive',
                f'@{rsp}', '-landroid', '-llog', '-laaudio', '-lmediandk', '-u', 'ANativeActivity_onCreate',
                '-Wl,-z,max-page-size=16384', '-Wl,--strip-debug'], log)
    say(f'phone: built {lib.name} ({lib.stat().st_size / 1e6:.0f} MB) in {round(time.time() - started)} s')
    return lib


# ---- the phone ---------------------------------------------------------------------------------------------------

def adb_path():
    """adb: B3_ADB, PATH, an Android SDK's platform-tools, else Google's platform-tools fetched into private/win."""
    exe = 'adb.exe' if sys.platform == 'win32' else 'adb'
    candidates = [os.environ.get('B3_ADB'), shutil.which('adb')]
    for sdk in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
                os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'),
                os.path.expanduser('~/Android/Sdk'), os.path.expanduser('~/Library/Android/sdk')):
        if sdk:
            candidates.append(os.path.join(sdk, 'platform-tools', exe))
    own = WIN / 'tools/platform-tools' / exe
    candidates.append(str(own))
    for c in candidates:
        if c and Path(c).is_file():
            return Path(c)
    url = PLATFORM_TOOLS.get('linux' if sys.platform.startswith('linux') else sys.platform)
    if not url:
        raise SetupError('no adb found: install Android platform-tools')
    say("phone: no adb on this computer; fetching Google's platform-tools (adb)")
    archive = WIN / 'tools/downloads/platform-tools.zip'
    download(url, archive)
    with zipfile.ZipFile(archive) as z:
        z.extractall(WIN / 'tools')
    if sys.platform != 'win32':
        own.chmod(0o755)
    return own


class Phone:
    def __init__(self, adb, serial):
        self.adb, self.serial = adb, serial

    def run(self, *args, check=True, capture=True, timeout=None):
        cmd = [str(self.adb), '-s', self.serial, *map(str, args)]
        r = subprocess.run(cmd, capture_output=capture, text=True, timeout=timeout)
        if check and r.returncode:
            raise SetupError(f'adb {" ".join(map(str, args))[:120]} failed: {(r.stdout or "") + (r.stderr or "")}'.strip())
        return r.stdout if capture else ''

    def shell(self, command, check=True):
        return self.run('shell', command, check=check)


def pick_phone(adb, serial):
    out = subprocess.run([str(adb), 'devices', '-l'], capture_output=True, text=True).stdout
    rows = [l.split() for l in out.splitlines()[1:] if l.strip()]
    ready = [r for r in rows if len(r) > 1 and r[1] == 'device']
    if serial:
        if not any(r[0] == serial for r in ready):
            raise SetupError(f'phone {serial} is not connected (adb devices: {", ".join(r[0] for r in rows) or "none"})')
        return Phone(adb, serial)
    if any(len(r) > 1 and r[1] == 'unauthorized' for r in rows):
        raise SetupError('the phone asks whether to allow USB debugging: tap Allow on it, then run this again')
    if not ready:
        raise SetupError('no phone found. On the phone: Settings > About phone > tap Build number 7 times, then '
                         'Developer options > USB debugging on; connect it by USB and allow this computer')
    if len(ready) > 1:
        names = ', '.join(f'{r[0]} ({next((x[6:] for x in r if x.startswith("model:")), "?")})' for r in ready)
        raise SetupError(f'more than one phone is connected: pick one with --serial ({names})')
    return Phone(adb, ready[0][0])


def apk_version(apk):
    """versionName from the file name the release gives it (Bayonetta3Recomp-<v>-android-arm64.apk), else None."""
    name = apk.name
    if name.startswith('Bayonetta3Recomp-') and name.endswith('-android-arm64.apk'):
        return name[len('Bayonetta3Recomp-'):-len('-android-arm64.apk')]
    return None


def install_app(phone, apk):
    installed = phone.shell(f'dumpsys package {PACKAGE} | grep -m1 versionName', check=False).strip()
    installed = installed.split('=', 1)[1] if '=' in installed else None
    want = apk_version(apk)
    if installed and want and installed == want:
        say(f'phone: the app {installed} is installed')
        return
    say(f'phone: installing {apk.name}' + (f' (over {installed})' if installed else ''))
    r = subprocess.run([str(phone.adb), '-s', phone.serial, 'install', '-r', str(apk)], capture_output=True, text=True)
    text = r.stdout + r.stderr
    if r.returncode or 'Success' not in text:
        if 'INSTALL_FAILED_ABORTED' in text or 'INSTALL_FAILED_USER_RESTRICTED' in text:
            raise SetupError('the phone did not allow the install. Some phones (vivo, Xiaomi, OPPO, realme) ask on '
                             'the phone to confirm an install over USB: watch the phone and confirm it when you run '
                             'this again. Xiaomi also needs Developer options > "Install via USB" on.')
        if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in text:
            raise SetupError('the app on the phone was signed by someone else (a build of your own?): uninstall '
                             '"Bayonetta 3 Recomp" on the phone first. Your saves and the game data in Internal '
                             'storage/Bayonetta 3 stay.')
        raise SetupError(f'adb install failed: {text.strip()[-400:]}')
    # The app reads shared storage: grant it the all-files access its first start would ask for.
    phone.shell(f'appops set {PACKAGE} MANAGE_EXTERNAL_STORAGE allow', check=False)


def remote_sizes(phone, folder):
    out = phone.shell(f'find "{folder}" -type f -exec stat -c "%s %n" {{}} + 2>/dev/null', check=False)
    sizes = {}
    for line in out.splitlines():
        size, _, path = line.partition(' ')
        if size.isdigit():
            sizes[path] = int(size)
    return sizes


def push_tree(phone, local, remote, label):
    """local/ into remote/ on the phone: only files missing there or of another size. A folder whose files mostly
    differ goes in one adb push (thousands of small shaders), the rest file by file."""
    files = [p for p in sorted(Path(local).rglob('*')) if p.is_file()]
    there = remote_sizes(phone, remote)
    todo = [p for p in files if there.get(f'{remote}/{p.relative_to(local).as_posix()}') != p.stat().st_size]
    total = sum(p.stat().st_size for p in todo)
    if not todo:
        say(f'phone: {label}: up to date ({len(files)} files)')
        return
    say(f'phone: {label}: copying {len(todo)} of {len(files)} files ({total / 1e9:.2f} GB)')
    started = time.time()
    if len(todo) > 200 and len(todo) > len(files) // 2:
        phone.shell(f'mkdir -p "{remote}"')
        phone.run('push', f'{local}/.', f'{remote}/', capture=False)
    else:
        sent = 0
        for i, p in enumerate(todo, 1):
            dest = f'{remote}/{p.relative_to(local).as_posix()}'
            phone.shell(f'mkdir -p "{dest.rsplit("/", 1)[0]}"')
            phone.run('push', p, dest)
            sent += p.stat().st_size
            if i % 10 == 0 or i == len(todo):
                rate = sent / max(1e-3, time.time() - started) / 1e6
                say(f'phone: {label}: {i}/{len(todo)} files, {sent / 1e9:.2f} of {total / 1e9:.2f} GB '
                    f'({rate:.0f} MB/s)')
    say(f'phone: {label}: done in {round(time.time() - started)} s')


def send_library(phone, lib):
    """The library to the launcher's socket (GameImport.java), which only adb's shell user can reach."""
    phone.shell(f'am start -n {PACKAGE}/.LauncherActivity', check=False)
    digest, size = sha256(lib), lib.stat().st_size
    port = None
    for attempt in range(20):
        time.sleep(1 if attempt else 2)
        port = phone.run('forward', 'tcp:0', f'localabstract:{PACKAGE}.import').strip()
        try:
            with socket.create_connection(('127.0.0.1', int(port)), timeout=30) as s:
                f = s.makefile('rb')
                if f.readline().strip() != b'READY 1':
                    raise OSError('no answer')
                say(f'phone: handing the game to the app ({size / 1e6:.0f} MB)')
                s.sendall(f'B3-LIB 1 {size} {digest}\n'.encode())
                with open(lib, 'rb') as data:
                    s.sendfile(data)
                answer = f.readline().decode(errors='replace').strip()
                if not answer.startswith('OK'):
                    raise SetupError(f'the app refused the game: {answer}')
                return
        except (OSError, ValueError):
            continue
        finally:
            if port:
                phone.run('forward', '--remove', f'tcp:{port}', check=False)
    raise SetupError('the app did not answer: unlock the phone, open "Bayonetta 3 Recomp" and run this again')


def phone_command(a):
    settings = b3_win.load(b3_win.SETTINGS, {})
    found = b3_win.tools(settings)
    apk = release_apk(a.apk)
    toolset = apk_toolset(apk)
    lib = build(found, toolset, a.jobs)
    if a.build_only:
        say(f'phone: built {lib}; not sent (--build-only)')
        return 0
    phone = pick_phone(adb_path(), a.serial)
    model = phone.shell('getprop ro.soc.model', check=False).strip()
    say(f'phone: {phone.serial}' + (f' ({model})' if model else ''))
    install_app(phone, apk)
    _, romfs = b3_win.local_layout(settings, 'b3')
    phone.shell(f'mkdir -p "{REMOTE}/user"')
    push_tree(phone, WIN / 'cpu/b3/analysis', f'{REMOTE}/analysis', 'code images')
    push_tree(phone, WIN / 'shaders/b3/spirv', f'{REMOTE}/spirv', 'shaders')
    push_tree(phone, romfs, f'{REMOTE}/extract/romfs', 'game files (RomFS)')
    send_library(phone, lib)
    say('phone: done. Open "Bayonetta 3 Recomp" on the phone and tap START.')
    return 0
