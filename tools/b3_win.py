#!/usr/bin/env python3
"""Bayonetta 3 on Windows, from your own dump to playing, without Git Bash.

    python tools/b3_win.py setup --dump D:\\dumps\\bayonetta3 [--program1 D:\\dumps\\bayonetta3\\program1]
    python tools/b3_win.py setup --status          # what is done, what is next
    python tools/b3_win.py play [runtime arguments...]

`setup` runs these steps in order and records each one in private/win/state.json, so an interrupted setup resumes
where it stopped (`--from <step>` reruns from a step, `--only <step>[,<step>]` runs just those):

    deps     find the build tools; fetch and build the pinned ones a release cannot assume (the Python packages, the
             ooz Kraken decoder, the Ryujinx 1.3.2 shader subset, SPIRV-Tools' spirv-val, libopus)
    verify   the dump's ExeFS modules and shader archives against config/bayonetta3-1.2.0*.json
    data     the dump's exefs/ and romfs/ into private/win/data/<program>, so this folder holds the whole game and can
             be moved: hard links on the same drive (no extra space, the dump stays whole; --data link, the
             default), a copy (--data copy), moved out of the dump (--data move), or left where it is (--data keep)
    cpu      the AArch64 -> C++ generation of every module (tools/prepare_bayonetta3.py --generate)
    shaders  extract, translate, verify and publish the game's shaders (tools/bayonetta3_shaders.py)
    build    the executables (CMake + Ninja + llvm-mingw) and the DLLs beside them
    link     the data roots the runtime reads, as directory junctions (no copies, no elevation); `play` makes them
             again when the folder was moved

Program 1 (the Bayonetta Origins prologue the hub's picture book starts) goes through cpu, shaders, build and link too
when --program1 names its folder (exefs/ and romfs/ from the 1.2.0 update, or main/sdk/main.npdm beside romfs/).
Without it the book is skipped and opening it ends the game, as on a dump without the update.

`play` reads private/b3.conf (config/b3.conf.example documents it; `key=value` becomes `NX_<KEY>`, as for
scripts/b3_play.sh) and runs the game. The picture book asks the runtime to start program 1 (it exits with code 86
and writes relaunch.txt); `play` then starts program 1 with what program 0 pushed to its user channel, and program 1
returns to program 0 the same way. Both programs share one save folder.

Everything this writes is under private/ (git-ignored): game-derived data never leaves your machine.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_data  # noqa: E402  (tools/b3_data.py: the data step, shared with Linux)

ROOT = Path(__file__).resolve().parents[1]
WIN = Path(os.environ.get('B3_WIN_ROOT') or ROOT / 'private/win')  # B3_WIN_ROOT: another tree (tests)
STATE = WIN / 'state.json'
SETTINGS = WIN / 'setup.json'
STEPS = ['deps', 'verify', 'data', 'cpu', 'shaders', 'build', 'link']
PROGRAMS = {  # program -> (version configuration, executable target, data root name)
    'b3': (ROOT / 'config/bayonetta3-1.2.0.json', 'bayonetta3', 'b3'),
    'prog1': (ROOT / 'config/bayonetta3-1.2.0-prog1.json', 'bayonetta3prog1', 'prog1'),
}
TRIPLET = 'x86_64-w64-mingw32'
# Pinned sources (docs/BUILD.md): the commits the shader pipeline was verified with.
OOZ = ('https://github.com/powzix/ooz.git', '05038060aa68f9187ae9923b2388ca8db40e58d1')
RYUJINX = ('https://github.com/enginefuture/bayonetta3-mobile.git', 'e5ce11694d380d70b53c4ec7c4f789b458058d3e',
           'third_party/blade2-mobile/dm/third_party/ryujinx')
SPIRV_TOOLS = ('https://github.com/KhronosGroup/SPIRV-Tools.git', 'v2026.1')
OPUS = ('https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz',
        '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1')
VULKAN_HEADERS = ('https://github.com/KhronosGroup/Vulkan-Headers.git', 'v1.4.357')
# The build toolchain, fetched when it is not installed (the official archives, hash-checked): with these a player
# needs only Python and a GPU driver. llvm-mingw 20261020 is the release every result in docs/WINDOWS-PORT was made with.
TOOLCHAIN = {
    'llvm_mingw': ('https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/'
                   'llvm-mingw-20261020-ucrt-x86_64.zip',
                   'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'),
    'cmake': ('https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip',
              '326c826486d46a23cf9d5a1c9ee3a567d427bdd02c4a34071937a2a675cff004'),
    'ninja': ('https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
              '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'),
    'git': ('https://github.com/git-for-windows/git/releases/download/v2.56.0.windows.4/MinGit-2.56.0.4-64-bit.zip',
            'e3bf78351fe777fba47ddee5950082f8a98bb0f02604fdc7ba3ee2cb68ffa877'),
    'dotnet': ('https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.403/dotnet-sdk-10.0.403-win-x64.zip',
               '7a114c318fa7f1d9822991b534b2814d0e78386ceda816b371f5141acefe741d87ccb606ede06cadac8f9ac5070e32b708a67'
               'bbd1619ff5e5a7858a27874ef48'),
}


class SetupError(Exception):
    pass


def say(message):
    print(f'b3: {message}', flush=True)


def load(path, default):
    try:
        return json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        return default


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    os.replace(tmp, path)


def sha256(path, algorithm='sha256'):
    h = hashlib.new(algorithm)
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    return h.hexdigest()


def run(cmd, log=None, cwd=None, env=None):
    """Run a command; its output goes to `log` (and the last lines to the console on failure)."""
    cmd = [str(c) for c in cmd]
    if log is None:
        p = subprocess.run(cmd, cwd=cwd, env=env)
        if p.returncode:
            raise SetupError(f'{Path(cmd[0]).name} failed (exit {p.returncode})')
        return
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('a', encoding='utf-8') as f:
        f.write(f'\n$ {subprocess.list2cmdline(cmd)}\n')
        f.flush()
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if p.returncode:
        tail = log.read_text(encoding='utf-8', errors='replace').splitlines()[-15:]
        raise SetupError(f'{Path(cmd[0]).name} failed (exit {p.returncode}); last lines of {log}:\n  ' +
                         '\n  '.join(tail))


def is_link(path):
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return False
    return path.is_symlink() or getattr(st, 'st_reparse_tag', 0) == 0xA0000003  # IO_REPARSE_TAG_MOUNT_POINT


def junction(link, target):
    """A directory junction link -> target (no Developer Mode, no elevation). An existing link to the same target is
    kept; one to another target is replaced (it is ours: everything under private/win/run was made here); a real
    directory in the way is an error, never deleted."""
    target = Path(target).resolve()
    if not target.is_dir():
        raise SetupError(f'missing directory: {target}')
    if is_link(link):
        if Path(os.path.realpath(link)) == target:
            return
        os.rmdir(link) if os.name == 'nt' else link.unlink()
    elif link.exists():
        raise SetupError(f'{link} is a real directory; move it away (it may hold your files) and run again')
    link.parent.mkdir(parents=True, exist_ok=True)
    if os.name == 'nt':
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)


# ---- the inputs ------------------------------------------------------------------------------------------------------

def child(folder, *names):
    """The first of `names` under folder, case-insensitively (dumps use exefs/ExeFS, romfs/RomFS)."""
    if not folder or not folder.is_dir():
        return None
    entries = {p.name.lower(): p for p in folder.iterdir()}
    for name in names:
        if name.lower() in entries:
            return entries[name.lower()]
    return None


def dump_layout(folder, program):
    """(exefs, romfs, game) of a dump folder: exefs/ + romfs/, or (program 1 as extracted by some tools) the ExeFS
    files beside romfs/. `game` is a folder holding both as ExeFS/RomFS for the shader tool (case-insensitive)."""
    folder = Path(folder)
    exefs = child(folder, 'exefs')
    romfs = child(folder, 'romfs')
    if exefs is None and child(folder, 'main') is not None:
        exefs = folder
    if exefs is None or romfs is None:
        raise SetupError(f'{folder} must hold exefs/ and romfs/ (the {program} dump)')
    return exefs, romfs


def check_dump(program, folder, log):
    config_path = PROGRAMS[program][0]
    config = json.loads(config_path.read_text(encoding='utf-8'))
    exefs, romfs = dump_layout(folder, program)
    bad = []
    for name, module in config['modules'].items():
        path = child(exefs, name)
        if path is None:
            bad.append(f'{name}: missing from {exefs}')
        elif sha256(path) != module['sha256']:
            bad.append(f'{name}: not the 1.2.0 module (sha256 {sha256(path)[:16]}..., want {module["sha256"][:16]}...)')
    if child(exefs, 'main.npdm') is None:
        bad.append(f'main.npdm: missing from {exefs}')
    sys.path.insert(0, str(ROOT / 'tools'))
    from bayonetta3_shaders import ARCHIVES  # the shader archives each program keeps its shaders in
    for name, want in ARCHIVES[config['title_id']].items():
        path = child(romfs, name)
        if path is None:
            bad.append(f'{name}: missing from {romfs}')
        elif sha256(path) != want:
            bad.append(f'{name}: does not match 1.2.0')
    if bad:
        raise SetupError(f'the {program} dump does not match Bayonetta 3 1.2.0:\n  ' + '\n  '.join(bad))
    with log.open('a', encoding='utf-8') as f:
        f.write(f'{program}: {exefs} and {romfs} match {config_path.name}\n')


# ---- the tools -------------------------------------------------------------------------------------------------------

def which(name, *extra):
    for candidate in extra:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    found = shutil.which(name)
    return Path(found) if found else None


def tools(settings):
    """Every external tool, from setup.json's `tools` (paths you gave with --tool), PATH, or what deps built."""
    given = settings.get('tools', {})
    t = WIN / 'tools'
    found = {
        'cxx': which(f'{TRIPLET}-clang++', given.get('cxx'),
                     given.get('llvm_mingw') and Path(given['llvm_mingw']) / 'bin' / f'{TRIPLET}-clang++.exe',
                     t / 'llvm-mingw/bin' / f'{TRIPLET}-clang++.exe'),
        'cmake': which('cmake', given.get('cmake'), t / 'cmake/bin/cmake.exe'),
        'ninja': which('ninja', given.get('ninja'), t / 'ninja/ninja.exe'),
        'git': which('git', given.get('git'), t / 'git/cmd/git.exe'),
        'dotnet': which('dotnet', given.get('dotnet'), r'C:\Program Files\dotnet\dotnet.exe', t / 'dotnet/dotnet.exe'),
        'spirv_val': which('spirv-val', given.get('spirv_val'),
                           t / 'SPIRV-Tools/build/tools/spirv-val.exe'),
        'decoder': which('kraken_worker', given.get('decoder'), t / 'kraken/kraken_worker.exe'),
        'python': which('python', given.get('python'), t / 'venv/Scripts/python.exe'),
    }
    found['cc'] = found['cxx'] and found['cxx'].with_name(found['cxx'].name.replace('clang++', 'clang'))
    found['ryujinx'] = Path(given['ryujinx']) if given.get('ryujinx') else t / 'ryujinx'
    found['vulkan'] = Path(given['vulkan']) if given.get('vulkan') else t / 'Vulkan-Headers'
    found['opus'] = Path(given['opus']) if given.get('opus') else t / 'opus/libopus.dll'
    return found


def build_env(found):
    """PATH with llvm-mingw's bin first (its clang, lld and the runtime DLLs), and the tools' own directories."""
    env = dict(os.environ)
    dirs = [found[k].parent for k in ('cxx', 'cmake', 'ninja', 'git') if found.get(k)]
    env['PATH'] = os.pathsep.join([str(d) for d in dirs] + [env.get('PATH', '')])
    # A temp folder of the setup's own: MSBuild failed here with "Access to the path ...\Temp\MSBuildTemp... is
    # denied" when the user's %TEMP% held a locked leftover. No MSBuild nodes outlive a step (a node left running
    # keeps the harness DLLs locked), and the .NET CLI stays quiet.
    tmp = WIN / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    env.update(TEMP=str(tmp), TMP=str(tmp), MSBUILDDISABLENODEREUSE='1', DOTNET_CLI_TELEMETRY_OPTOUT='1',
               DOTNET_NOLOGO='1')
    return env


def fetch_git(url, ref, dest, log, env, sparse=None):
    if (dest / '.git').exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.partial')
    if tmp.exists():
        shutil.rmtree(tmp)
    git = shutil.which('git', path=env['PATH'])
    if sparse:
        run([git, 'clone', '--filter=blob:none', '--no-checkout', url, tmp], log, env=env)
        run([git, '-C', tmp, 'sparse-checkout', 'init', '--cone'], log, env=env)
        run([git, '-C', tmp, 'sparse-checkout', 'set', sparse], log, env=env)
        run([git, '-C', tmp, 'checkout', '--detach', ref], log, env=env)
    elif len(ref) == 40:
        run([git, 'clone', '--filter=blob:none', url, tmp], log, env=env)
        run([git, '-C', tmp, 'checkout', '--detach', ref], log, env=env)
    else:
        run([git, 'clone', '--depth', '1', '--branch', ref, url, tmp], log, env=env)
    os.replace(tmp, dest)


def fetch_archive(name, log):
    """A pinned toolchain archive (TOOLCHAIN) into private/win/tools/<folder>: downloaded, hash-checked, unpacked
    beside and renamed into place, so an interrupted fetch never leaves a half tool that looks installed."""
    url, want = TOOLCHAIN[name]
    folder = WIN / 'tools' / {'llvm_mingw': 'llvm-mingw'}.get(name, name)
    archive = WIN / 'tools/downloads' / Path(url).name
    archive.parent.mkdir(parents=True, exist_ok=True)
    algorithm = 'sha512' if len(want) == 128 else 'sha256'
    if not archive.exists() or sha256(archive, algorithm) != want:
        say(f'deps: downloading {archive.name}')
        part = archive.with_suffix('.part')
        urllib.request.urlretrieve(url, part)
        if sha256(part, algorithm) != want:
            part.unlink()
            raise SetupError(f'{archive.name} does not match its pinned {algorithm}; not used')
        os.replace(part, archive)
    import zipfile
    staging = folder.with_name(folder.name + '.partial')
    if staging.exists():
        shutil.rmtree(staging)
    with zipfile.ZipFile(archive) as z:
        z.extractall(staging)
    entries = list(staging.iterdir())
    source = entries[0] if len(entries) == 1 and entries[0].is_dir() else staging  # a zip with one top folder
    if folder.exists():
        shutil.rmtree(folder)
    os.replace(source, folder)
    if staging.exists():
        shutil.rmtree(staging)
    with log.open('a', encoding='utf-8') as f:
        f.write(f'fetched {url} ({algorithm} {want[:16]}...) into {folder}\n')


def step_deps(settings, log):
    found = tools(settings)
    names = {'cxx': 'llvm_mingw', 'cmake': 'cmake', 'ninja': 'ninja', 'git': 'git', 'dotnet': 'dotnet'}
    missing = [k for k in names if not found[k]]
    if missing and os.name != 'nt':
        raise SetupError(f'install these and run setup again: {", ".join(names[k] for k in missing)}')
    for k in missing:  # Windows: the pinned official archive (or name your own with --tool <name>=<path>)
        fetch_archive(names[k], log)
    found = tools(settings)
    if any(not found[k] for k in names):
        raise SetupError(f'still missing: {", ".join(names[k] for k in names if not found[k])}')
    env = build_env(found)
    t = WIN / 'tools'
    # Python packages for the recompiler (lz4, capstone) in a venv of their own. lz4 4.4.4 (the pin) has no Windows
    # wheel for current Pythons and needs MSVC to build; 4.4.5 is the same lz4.block the recompiler calls.
    venv = t / 'venv/Scripts/python.exe'
    if not venv.exists():
        say('deps: Python packages (venv)')
        run([sys.executable, '-m', 'venv', t / 'venv'], log)
        reqs = (ROOT / 'requirements-bayonetta3.txt').read_text(encoding='utf-8')
        pins = [line.strip() for line in reqs.splitlines() if line.strip() and not line.startswith('#')]
        pins = ['lz4==4.4.5' if p.startswith('lz4==') and os.name == 'nt' else p for p in pins]
        run([venv, '-m', 'pip', 'install', '--disable-pip-version-check', '--only-binary=:all:', *pins], log)
    if not found['vulkan'].joinpath('include/vulkan/vulkan.h').exists():
        say('deps: Vulkan headers')
        fetch_git(*VULKAN_HEADERS, found['vulkan'], log, env)
    if not found['decoder']:
        say('deps: the Kraken decoder (ooz)')
        ooz = t / 'ooz'
        fetch_git(*OOZ, ooz, log, env)
        out = t / 'kraken'
        out.mkdir(parents=True, exist_ok=True)
        run([found['cxx'], '-O2', '-w', '-include', 'sys/stat.h', '-Dmain=ooz_cli_main', '-c', ooz / 'kraken.cpp',
             '-o', out / 'kraken.o'], log, env=env)
        run([found['cxx'], '-O2', '-w', '-static', ROOT / 'tools/bayonetta3_shaders/kraken_worker_ooz.cpp',
             out / 'kraken.o', ooz / 'bitknit.cpp', ooz / 'lzna.cpp', '-o', out / 'kraken_worker.exe'], log, env=env)
        run([venv, ROOT / 'tools/test_kraken_worker.py', '--worker', out / 'kraken_worker.exe'], log, env=env)
    ryu = found['ryujinx']
    if not (ryu / 'src/Ryujinx.Graphics.Shader/bin/Release/net10.0/Spv.Generator.dll').exists():
        say('deps: the Ryujinx 1.3.2 shader translator')
        checkout = t / 'bayonetta3-mobile'
        fetch_git(RYUJINX[0], RYUJINX[1], checkout, log, env, sparse=RYUJINX[2])
        if not settings.get('tools', {}).get('ryujinx'):
            ryu = checkout / RYUJINX[2]
            settings.setdefault('tools', {})['ryujinx'] = str(ryu)
            save(SETTINGS, settings)
        run([found['dotnet'], 'build', ryu / 'src/Ryujinx.Graphics.Shader/Ryujinx.Graphics.Shader.csproj',
             '-c', 'Release', '-p:RunAnalyzers=false'], log, env=env)
    if not found['spirv_val']:
        say('deps: spirv-val (SPIRV-Tools, about 10 minutes)')
        src = t / 'SPIRV-Tools'
        fetch_git(*SPIRV_TOOLS, src, log, env)
        run([venv, src / 'utils/git-sync-deps'], log, cwd=src, env=env)
        run([found['cmake'], '-S', src, '-B', src / 'build', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_C_COMPILER={found["cc"]}', f'-DCMAKE_CXX_COMPILER={found["cxx"]}', '-DSPIRV_SKIP_TESTS=ON',
             '-DSPIRV_WERROR=OFF', '-DCMAKE_EXE_LINKER_FLAGS=-static'], log, env=env)
        run([found['cmake'], '--build', src / 'build', '--target', 'spirv-val'], log, env=env)
    if not found['opus'].exists():
        say('deps: libopus')
        work = t / 'opus-src'
        work.mkdir(parents=True, exist_ok=True)
        tarball = work / Path(OPUS[0]).name
        if not tarball.exists() or sha256(tarball) != OPUS[1]:
            urllib.request.urlretrieve(OPUS[0], tarball)
            if sha256(tarball) != OPUS[1]:
                raise SetupError(f'{tarball.name} does not match its pinned sha256')
        import tarfile
        with tarfile.open(tarball) as tar:
            tar.extractall(work, filter='data')
        src = work / tarball.name[:-len('.tar.gz')]
        run([found['cmake'], '-S', src, '-B', src / 'build-win', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_C_COMPILER={found["cc"]}', '-DOPUS_BUILD_SHARED_LIBRARY=ON', '-DOPUS_BUILD_TESTING=OFF',
             '-DOPUS_BUILD_PROGRAMS=OFF'], log, env=env)
        run([found['cmake'], '--build', src / 'build-win'], log, env=env)
        found['opus'].parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src / 'build-win/libopus.dll', found['opus'])
        shutil.copyfile(src / 'COPYING', found['opus'].with_name('COPYING.opus'))
    found = tools(settings)
    gaps = [k for k in ('spirv_val', 'decoder', 'python') if not found[k]]
    if gaps:
        raise SetupError(f'still missing after deps: {", ".join(gaps)}')
    if os.name == 'nt':  # the cutscenes are VP9 WebM, decoded by Media Foundation (runtime/src/movie_win.cpp)
        probe = subprocess.run(['powershell', '-NoProfile', '-Command',
                                '(Get-AppxPackage -Name Microsoft.VP9VideoExtensions).Version'],
                               capture_output=True, text=True)
        if probe.returncode == 0 and not probe.stdout.strip():
            say('warning: the "VP9 Video Extensions" (Microsoft Store, free) are not installed: '
                'without them the cutscenes stay blank')


# ---- the steps that make game data -----------------------------------------------------------------------------------

def programs(settings):
    return ['b3'] + (['prog1'] if settings.get('program1') else [])


def folder_of(settings, program):
    return Path(settings['dump'] if program == 'b3' else settings['program1'])


def local_layout(settings, program):
    """(exefs, romfs) the later steps read: private/win/data/<program> once the data step brought them there, else the
    dump's own folders."""
    here = WIN / 'data' / program
    if (here / 'complete.json').exists():
        return here / 'exefs', here / 'romfs'
    return dump_layout(folder_of(settings, program), program)


def step_data(settings, log):
    """The dump's exefs/ and romfs/ into private/win/data/<program> (tools/b3_data.py, shared with Linux)."""
    mode = settings.get('data_mode', 'link')
    for program in programs(settings):
        here = WIN / 'data' / program
        if (here / 'complete.json').exists():
            continue
        if mode == 'keep':
            say(f'data: {program} stays in {folder_of(settings, program)} (--data keep): this folder needs the dump')
            continue
        exefs, romfs = dump_layout(folder_of(settings, program), program)
        say(f'data: {program} by {mode} into {here}')
        b3_data.bring_dump(exefs, romfs, here, mode, say=say)


def step_verify(settings, log):
    for program in programs(settings):
        say(f'verify: {program}')
        check_dump(program, folder_of(settings, program), log)


def step_cpu(settings, log):
    found = tools(settings)
    for program in programs(settings):
        out = WIN / 'cpu' / program
        if (out / 'manifest.json').exists() and (out / 'gen').is_dir():
            continue  # prepare_bayonetta3.py checks ownership and identity; a finished one is not redone
        say(f'cpu: generating {program} (10-40 minutes)')
        exefs, romfs = local_layout(settings, program)
        run([found['python'], ROOT / 'tools/prepare_bayonetta3.py', '--generate', '--config', PROGRAMS[program][0],
             '--exefs', exefs, '--romfs', romfs, '--output', out, '--python', found['python']],
            log, env=build_env(found))


def step_shaders(settings, log):
    found = tools(settings)
    env = build_env(found)
    ryu = found['ryujinx']
    for program in programs(settings):
        published = WIN / 'shaders' / program
        if (published / 'spirv/results.tsv').exists():
            continue
        say(f'shaders: {program} (extract, translate, verify: 30-90 minutes)')
        exefs, romfs = local_layout(settings, program)
        game = WIN / 'game' / program  # ExeFS/ + RomFS/ as the shader tool expects them
        junction(game / 'ExeFS', exefs)
        junction(game / 'RomFS', romfs)
        work = WIN / 'shader-work' / program
        if work.exists():
            shutil.rmtree(work)  # ours; translate refuses a used work directory
        common = ['--config', PROGRAMS[program][0], '--work', work]
        tool = [found['python'], ROOT / 'tools/bayonetta3_shaders.py']
        run(tool + ['extract', '--game', game, '--decoder', found['decoder']] + common, log, env=env)
        run(tool + ['build', '--dotnet', found['dotnet'], '--ryu-shader', ryu / 'src/Ryujinx.Graphics.Shader',
                    '--ryu-lib', ryu / 'src/Ryujinx.Graphics.Shader/bin/Release/net10.0'] + common, log, env=env)
        run(tool + ['translate', '--dotnet', found['dotnet'], '--spirv-val', found['spirv_val']] + common,
            log, env=env)
        run(tool + ['verify', '--cxx', found['cxx'], '--spirv-val', found['spirv_val']] + common, log, env=env)
        if published.exists():
            shutil.rmtree(published)
        run(tool + ['publish', '--publish-dir', published] + common, log, env=env)


def step_build(settings, log, jobs):
    found = tools(settings)
    env = build_env(found)
    for program in programs(settings):
        build = WIN / 'build' / program
        say(f'build: {PROGRAMS[program][1]}.exe (the generated code: 10-30 minutes the first time)')
        if not (build / 'build.ninja').exists():
            run([found['cmake'], '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 f'-DNX_GAME={PROGRAMS[program][1]}', f'-DNX_GEN_DIR={(WIN / "cpu" / program / "gen").as_posix()}',
                 f'-DCMAKE_C_COMPILER={found["cc"]}', f'-DCMAKE_CXX_COMPILER={found["cxx"]}',
                 f'-DNX_VULKAN_SDK={found["vulkan"].as_posix()}', '-DNX_RECOMP_OPT=2'], log, env=env)
        run([found['cmake'], '--build', build, '--target', PROGRAMS[program][1]] +
            (['--parallel', str(jobs)] if jobs else []), log, env=env)
        runtime = found['cxx'].parent.parent / TRIPLET / 'bin'  # the target's own (bin/ holds the host's)
        for dll in ('libc++.dll', 'libunwind.dll'):  # llvm-mingw's C++ runtime
            shutil.copyfile(runtime / dll if (runtime / dll).exists() else found['cxx'].parent / dll, build / dll)
        shutil.copyfile(found['opus'], build / 'libopus.dll')


def data_roots():
    return WIN / 'run/b3', WIN / 'run/prog1'


def step_link(settings, log):
    b3_root, prog1_root = data_roots()
    for program in programs(settings):
        root = b3_root if program == 'b3' else prog1_root
        _, romfs = local_layout(settings, program)
        junction(root / 'analysis', WIN / 'cpu' / program / 'analysis')
        junction(root / 'spirv', WIN / 'shaders' / program / 'spirv')
        junction(root / 'extract/romfs', romfs)
    (b3_root / 'user').mkdir(parents=True, exist_ok=True)
    if settings.get('program1'):
        junction(prog1_root / 'user', b3_root / 'user')  # one application, one save, as on the console
    say(f'link: data in {b3_root}; your saves are in {b3_root / "user"}')


def setup(a):
    settings = load(SETTINGS, {})
    if a.dump:
        settings['dump'] = str(Path(a.dump).resolve())
    had_program1 = bool(settings.get('program1'))
    if a.program1:
        settings['program1'] = str(Path(a.program1).resolve())
    if a.data:
        settings['data_mode'] = a.data
    if not a.program1 and 'program1' not in settings and settings.get('dump'):
        p1 = child(Path(settings['dump']), 'program1')  # Witch Time's layout: the dump's program1/ folder
        if p1:
            settings['program1'] = str(p1.resolve())
    # Program 1 added to a finished setup: the steps run again from verify. Each one skips what a program already has
    # (its generated code, shaders, data), so only program 1 is made.
    program1_added = not had_program1 and bool(settings.get('program1'))
    for item in a.tool or []:
        name, _, value = item.partition('=')
        settings.setdefault('tools', {})[name] = str(Path(value).resolve())
    state = load(STATE, {})
    if a.status:
        for step in STEPS:
            entry = state.get(step)
            print(f'  {step:8} {"done " + entry["at"] if entry else "pending"}')
        print(f'  dump     {settings.get("dump", "(not set: --dump)")}')
        print(f'  program1 {settings.get("program1", "(not set: the picture book is skipped)")}')
        return 0
    if 'dump' not in settings:
        raise SetupError('name your dump folder: setup --dump <folder with exefs/ and romfs/>')
    save(SETTINGS, settings)
    chosen = STEPS
    if a.only:
        chosen = [s for s in a.only.split(',') if s]
        unknown = set(chosen) - set(STEPS)
        if unknown:
            raise SetupError(f'unknown steps: {", ".join(sorted(unknown))} (steps: {", ".join(STEPS)})')
    elif a.from_step:
        chosen = STEPS[STEPS.index(a.from_step):]
    log = WIN / 'logs' / f'setup-{time.strftime("%Y%m%d-%H%M%S")}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    say(f'log: {log}')
    if program1_added and not a.only and not a.from_step:
        say('setup: program 1 (the picture book) is new: making it')
        for step in STEPS[STEPS.index('verify'):]:
            state.pop(step, None)
    for step in STEPS:
        if step not in chosen or (not a.only and not a.from_step and step in state):
            continue
        started = time.time()
        if step == 'build':
            step_build(settings, log, a.jobs)
        else:
            globals()[f'step_{step}'](settings, log)
        state[step] = {'at': time.strftime('%Y-%m-%d %H:%M:%S'), 'seconds': round(time.time() - started)}
        if step in FINGERPRINTED:
            state[step]['fingerprint'] = fingerprint(step)
        save(STATE, state)
        say(f'{step}: done in {round(time.time() - started)} s')
    say('setup complete: python tools/b3_win.py play')
    return 0


# ---- update ----------------------------------------------------------------------------------------------------------

# What each long step's output depends on in this checkout: when a new version changes these files, `update` redoes
# the step; when it does not, the step's output is kept and only what changed is rebuilt.
FINGERPRINTED = {
    'cpu': ['recomp/*.py', 'tools/prepare_bayonetta3.py', 'config/bayonetta3-1.2.0*.json'],
    'shaders': ['tools/bayonetta3_shaders.py', 'tools/bayonetta3_shaders/**/*', 'tools/shaders/**/*',
                'tools/gen_shaders.py'],
}
NOT_FINGERPRINTED = {'recomp/b3_hooks.py'}  # applied to the generated code in place (tools/refresh_b3_hooks.py)


def fingerprint(step):
    h = hashlib.sha256()
    files = sorted({f for pattern in FINGERPRINTED[step] for f in ROOT.glob(pattern)
                    if f.is_file() and '__pycache__' not in f.parts})
    for f in files:
        rel = f.relative_to(ROOT).as_posix()
        if rel in NOT_FINGERPRINTED:
            continue
        h.update(rel.encode() + bytes(1) + f.read_bytes() + bytes(1))
    return h.hexdigest()


def update_plan(state):
    """What `update` will do: [(step, why)]."""
    plan = []
    for step in FINGERPRINTED:
        recorded = state.get(step, {}).get('fingerprint')
        if recorded and recorded != fingerprint(step):
            plan.append((step, 'its tools changed: made again'))
    if not any(s == 'cpu' for s, _ in plan):
        plan.append(('hooks', 'new runtime hooks applied to the generated code in place'))
    plan.append(('build', 'only the changed files compile again'))
    plan.append(('link', 'the data roots'))
    return plan


def update(a):
    """After a new version of this checkout (a pull, a new release unpacked over it): bring the build up to date
    without a full setup. Shaders and the generated C++ are kept unless the tools that make them changed."""
    settings, state = load(SETTINGS, {}), load(STATE, {})
    if not all(step in state for step in STEPS):
        raise SetupError('not set up yet: python tools/b3_win.py setup --dump <your dump folder>')
    if not settings.get('program1') and settings.get('dump') and child(Path(settings['dump']), 'program1'):
        # The picture book's program put into the dump after the setup: setup makes it (verify .. link, program 1).
        say('update: the dump now has program1/ (the picture book): setting it up first')
        rc = setup(argparse.Namespace(dump=None, program1=None, tool=None, only=None, from_step=None, status=False,
                                      data=None, jobs=a.jobs))
        if rc:
            return rc
        state = load(STATE, {})
    plan = update_plan(state)
    for step, why in plan:
        say(f'update: {step}: {why}')
    if a.check:
        return 0
    log = WIN / 'logs' / f'update-{time.strftime("%Y%m%d-%H%M%S")}.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    for step, _ in plan:
        started = time.time()
        if step in FINGERPRINTED:
            for program in programs(settings):  # set the old output aside; the step makes it again
                old = WIN / ('cpu' if step == 'cpu' else 'shaders') / program
                if old.exists():
                    old.rename(old.with_name(f'{program}.before-{stamp}'))
            globals()[f'step_{step}'](settings, log)
            for program in programs(settings):
                shutil.rmtree(WIN / ('cpu' if step == 'cpu' else 'shaders') / f'{program}.before-{stamp}',
                              ignore_errors=True)
        elif step == 'hooks':
            found = tools(settings)
            exefs, _ = local_layout(settings, 'b3')
            run([found['python'], ROOT / 'tools/refresh_b3_hooks.py', WIN / 'cpu/b3/gen', '--nso', exefs / 'main',
                 '--apply', '--backup', WIN / f'hooks-backup-{stamp}'], log)
        elif step == 'build':
            step_build(settings, log, a.jobs)
        else:
            step_link(settings, log)
        if step in STEPS:
            state[step] = {'at': time.strftime('%Y-%m-%d %H:%M:%S'), 'seconds': round(time.time() - started)}
            if step in FINGERPRINTED:
                state[step]['fingerprint'] = fingerprint(step)
            save(STATE, state)
        say(f'update: {step}: done in {round(time.time() - started)} s')
    for step in FINGERPRINTED:  # a setup from before fingerprints: what it was made from is what is here now
        state[step].setdefault('fingerprint', fingerprint(step))
    save(STATE, state)
    say('update complete: python tools/b3_win.py play')
    return 0


# ---- play ------------------------------------------------------------------------------------------------------------

ZERO_IS_A_VALUE = {'NX_PAD_DEADZONE', 'NX_PAD_TRIGGER', 'NX_PAD_INDEX', 'NX_FSR_SHARP', 'NX_MOUSE_CAMERA'}


def conf_env(path):
    """private/b3.conf as environment: `key=value` -> NX_<KEY>, the same rules as scripts/b3_play.sh (false, off, no
    and empty unset a key; so does 0, except for the numeric settings where 0 is a value)."""
    env = {}
    if not path.exists():
        return env
    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.split('#', 1)[0].strip()
        if '=' not in line:
            continue
        key, value = (s.strip() for s in line.split('=', 1))
        if not key.replace('_', '').isalnum():
            continue
        name = key if key.startswith('NX_') else 'NX_' + key.upper()
        if value.lower() in ('false', 'off', 'no', '') or (value == '0' and name.upper() not in ZERO_IS_A_VALUE):
            continue
        env[name] = value
    return env


BOOK_NOT_BUILT = (
    'The Old Picture Book starts a second program of the game, the Bayonetta Origins prologue (program 1 of the '
    '1.2.0 update), and it was not built: your dump had no program1 folder when you set up.\n\n'
    'Put program 1 of your own 1.2.0 update in a program1 folder beside exefs and romfs (its own exefs and romfs), '
    'then run Rebuild in the launcher (or: python tools\\b3_win.py setup --program1 <folder>).\n\n'
    'The game starts again now; your save is kept.\n\n'
    '“古老的绘本”会启动游戏的第二个程序（1.2.0 更新里的 program 1，Bayonetta Origins 序章），而它没有被构建：'
    '设置时你的 dump 里没有 program1 文件夹。\n\n'
    '把你自己的 1.2.0 更新里的 program 1 放进 exefs、romfs 旁边的 program1 文件夹（内含它自己的 exefs 和 romfs），'
    '再在启动器里点 Rebuild（或运行 python tools\\b3_win.py setup --program1 <文件夹>）。\n\n'
    '游戏现在会重新启动，存档不受影响。')


def book_not_built():
    """A message box over the closed game: why the picture book returned to the title (Windows; elsewhere the log)."""
    if os.name != 'nt' or os.environ.get('NX_HEADLESS'):
        return
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, BOOK_NOT_BUILT, 'Bayonetta 3: the Old Picture Book', 0x40 | 0x40000)
    except Exception:
        pass


def links_ok(settings):
    """Whether every data-root junction still reaches what it should: junctions hold absolute paths, so after the
    folder was moved (or the drive letter changed) they point at the old place."""
    b3_root, prog1_root = data_roots()
    for program in programs(settings):
        root = b3_root if program == 'b3' else prog1_root
        expect = {'analysis': WIN / 'cpu' / program / 'analysis', 'spirv': WIN / 'shaders' / program / 'spirv',
                  'extract/romfs': local_layout(settings, program)[1]}
        for name, target in expect.items():
            link = root / name
            if not is_link(link) or Path(os.path.realpath(link)) != Path(target).resolve():
                return False
    return True


def play(a):
    settings = load(SETTINGS, {})
    b3_root, prog1_root = data_roots()
    if settings.get('dump') and not links_ok(settings):
        say('play: the folder moved (or a link broke): linking the data roots again')
        step_link(settings, None)
    exes = {0: WIN / 'build/b3/bayonetta3.exe', 1: WIN / 'build/prog1/bayonetta3prog1.exe'}
    roots = {0: b3_root, 1: prog1_root}
    if not exes[0].exists():
        raise SetupError('not set up yet: python tools/b3_win.py setup --dump <your dump folder>')
    conf = Path(os.environ.get('B3_CONFIG') or ROOT / 'private/b3.conf')  # B3_CONFIG: another file, as for b3_play.sh
    if not conf.exists():
        shutil.copyfile(ROOT / 'config/b3.conf.example', conf)
        say(f'created {conf} from the template (edit it for resolution, fullscreen, the controller ...)')
    base = dict(os.environ)
    for name in ('NX_HEADLESS', 'NX_MUTE'):  # leftovers of a measurement shell; the settings file decides
        base.pop(name, None)
    base.update(conf_env(conf))
    base['NX_SETTINGS_FILE'] = str(conf.resolve())  # the in-game settings panel (F1) saves its changes here
    log = Path(base.get('NX_LOG') or WIN / 'logs/game.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    program, switches = 0, 0
    while True:
        env = dict(base, NX_DATA_ROOT=str(roots[program]),
                   NX_LOG=str(log if program == 0 else log.with_name(log.stem + '-prog1' + log.suffix)))
        say(f'starting program {program} ({exes[program].name}); log {env["NX_LOG"]}')
        rc = subprocess.run([str(exes[program])] + a.args, env=env, cwd=str(exes[program].parent)).returncode
        relaunch = roots[program] / 'relaunch.txt'
        if rc != 86 or switches >= 16:
            return rc
        switches += 1
        kind, value = (relaunch.read_text(encoding='utf-8').split() + ['0', '0'])[:2] if relaunch.exists() else ('0', '0')
        relaunch.unlink(missing_ok=True)
        target = 1 if (kind, value) == ('0', '1') else 0
        if target == 1 and not exes[1].exists():
            say('the picture book needs program 1: setup --program1 <folder>; returning to the game')
            book_not_built()  # the game window just closed: say why, instead of what looks like a crash (issue #2)
            target = 0
        channel = roots[program] / 'user_channel.bin'
        if channel.exists():
            shutil.copyfile(channel, roots[target] / 'launch_parameter.txt')
        say(f'program {program} asked for program {target} (kind {kind}, value {value})')
        program = target


def first_run(a):
    """The release's double-click entry (Bayonetta3Recomp.cmd): set up from a dump the player picks, then the launcher.
    A finished setup goes straight to the launcher."""
    state = load(STATE, {})
    if not all(step in state for step in STEPS):
        settings = load(SETTINGS, {})
        dump = a.dump or settings.get('dump')
        if not dump:
            say('Pick the folder that holds your Bayonetta 3 1.2.0 dump (exefs\\ and romfs\\).')
            try:
                import tkinter
                from tkinter import filedialog
                root = tkinter.Tk()
                root.withdraw()
                dump = filedialog.askdirectory(title='Your Bayonetta 3 1.2.0 dump: the folder with exefs and romfs')
                root.destroy()
            except Exception:  # no Tk: ask on the console
                dump = input('Dump folder: ').strip().strip('"')
        if not dump:
            raise SetupError('no dump folder chosen')
        dump_layout(Path(dump), 'b3')  # says what is missing before an hour of setup starts
        say('Setting up: this builds the game from your dump on this PC (about an hour the first time; it resumes '
            'where it stopped if you close this window).')
        rc = setup(argparse.Namespace(dump=dump, program1=None, tool=None, only=None, from_step=None, status=False,
                                      data=a.data, jobs=0))
        if rc:
            return rc
    if a.no_launcher:
        return 0
    return subprocess.call([sys.executable, str(ROOT / 'tools/b3_launcher.py')], cwd=str(ROOT))


# What a release may hold: the repository's own tracked files, never anything derived from the game.
PACKAGE_REFUSED = ('.pkz', '.nso', '.nca', '.nsp', '.xci', '.keys', '.npdm', '.spv', '.bk2', '.webm')
LAUNCH_CMD = r'''@echo off
rem Bayonetta 3 Recomp: double-click for the launcher (Set up from your dump the first time, then Start game).
rem The console route: Bayonetta3Recomp.cmd console (setup in this window, then the launcher).
setlocal
cd /d "%~dp0"
if /i not "%~1"=="console" if exist "python\pythonw.exe" (
    start "" "python\pythonw.exe" tools\b3_launcher.py
    exit /b 0
)
if /i "%~1"=="console" shift
set PY=
if exist "python\python.exe" set PY=python\python.exe
if not defined PY if defined B3_PYTHON set PY=%B3_PYTHON%
if not defined PY where py >nul 2>nul && set PY=py -3
if not defined PY where python >nul 2>nul && set PY=python
if not defined PY (
    echo No Python 3 found: install it from python.org ^(with Tcl/Tk^) or use a release that bundles it.
    pause
    exit /b 1
)
%PY% tools\b3_win.py first-run
if errorlevel 1 pause
endlocal
'''


def package_files():
    """The tracked files of this checkout (git ls-files), checked: nothing under private/, no game formats."""
    out = subprocess.run(['git', 'ls-files', '-z'], cwd=str(ROOT), capture_output=True, check=True).stdout
    files = [f for f in out.decode('utf-8').split('\0') if f]
    bad = [f for f in files if f.startswith('private/') or f.lower().endswith(PACKAGE_REFUSED)]
    if bad:
        raise SetupError('refusing to package files that look game-derived: ' + ', '.join(bad[:5]))
    return files


def package(a):
    import zipfile
    version = a.version or subprocess.run(['git', 'describe', '--tags', '--always', '--dirty'], cwd=str(ROOT),
                                          capture_output=True, text=True).stdout.strip() or 'dev'
    name = f'Bayonetta3Recomp-{version}-windows-x64'
    out_dir = Path(a.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    zpath = out_dir / f'{name}.zip'
    files = package_files()
    count = 0
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            src = ROOT / f
            if src.is_file():
                z.write(src, f'{name}/{f}')
                count += 1
        z.writestr(f'{name}/Bayonetta3Recomp.cmd', LAUNCH_CMD.replace('\n', '\r\n'))
        if a.python:
            py = Path(a.python).resolve()
            if not (py / 'python.exe').exists():
                raise SetupError(f'{py} has no python.exe')
            for path in sorted(py.rglob('*')):
                if path.is_file() and '__pycache__' not in path.parts and 'site-packages' not in path.parts:
                    z.write(path, f'{name}/python/{path.relative_to(py).as_posix()}')
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    zpath.with_name(zpath.name + '.sha256').write_text(f'{digest}  {zpath.name}\n', encoding='utf-8')
    say(f'package: {zpath} ({count} files{", with Python" if a.python else ""}, '
        f'{zpath.stat().st_size / 1e6:.1f} MB), sha256 {digest}')
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    s = sub.add_parser('setup', help='build everything from your dump (resumable)')
    s.add_argument('--dump', help='folder with exefs/ and romfs/ of Bayonetta 3 1.2.0')
    s.add_argument('--program1', help="folder with program 1's exefs/ and romfs/ (default: <dump>/program1)")
    s.add_argument('--tool', action='append', help='name=path of a tool to use instead of PATH '
                   '(llvm_mingw, cxx, cmake, ninja, git, dotnet, spirv_val, decoder, python, ryujinx, vulkan, opus)')
    s.add_argument('--only', help='comma-separated steps to run (again)')
    s.add_argument('--from', dest='from_step', choices=STEPS, help='run again from this step on')
    s.add_argument('--status', action='store_true', help='show what is done')
    s.add_argument('--data', choices=['link', 'copy', 'move', 'keep'],
                   help='how the data step brings the dump here (default link: hard links, no extra space)')
    s.add_argument('--jobs', type=int, default=0, help='parallel compile jobs (default: all cores)')
    s.set_defaults(func=setup)
    q = sub.add_parser('play', help='run the game (and program 1 when the picture book asks for it)')
    q.add_argument('args', nargs=argparse.REMAINDER, help='runtime arguments')
    q.set_defaults(func=play)
    u = sub.add_parser('update', help='after a new version: rebuild only what changed (keeps shaders and C++)')
    u.add_argument('--check', action='store_true', help='say what would be done')
    u.add_argument('--jobs', type=int, default=0, help='parallel compile jobs (default: all cores)')
    u.set_defaults(func=update)
    f = sub.add_parser('first-run', help="a release's double-click entry: setup from a dump you pick, then the launcher")
    f.add_argument('--dump', help='the dump folder (default: a folder picker)')
    f.add_argument('--data', choices=['link', 'copy', 'move', 'keep'], help='as for setup')
    f.add_argument('--no-launcher', action='store_true', help='stop after setup')
    f.set_defaults(func=first_run)
    k = sub.add_parser('package', help='a release zip of this checkout (tracked files only, no game data)')
    k.add_argument('--out', default=str(ROOT / 'dist'), help='where the zip goes (default: dist/)')
    k.add_argument('--version', help='the name in the zip (default: git describe)')
    k.add_argument('--python', help='a Python install folder to bundle as python/ (python.exe, Lib, DLLs, tcl)')
    k.set_defaults(func=package)
    a = p.parse_args(argv)
    try:
        return a.func(a)
    except SetupError as e:
        print(f'b3: error: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
