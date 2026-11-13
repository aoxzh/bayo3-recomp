#!/usr/bin/env python3
"""The release toolset: every tool a player's setup needs, built once on the release machine (CI), so the player's
own machine only turns their dump into the game and never fetches or builds a tool.

    python tools/build_toolset.py --out toolset [--work private/toolset-work] [--jobs N]

On Windows (needs llvm-mingw, CMake, Ninja, Git and a .NET 10 SDK on PATH, or the ones b3_win.py setup fetched):
    toolset/bin/          spirv-val.exe, libopus.dll
    toolset/shader-harness/ShaderHarness.exe   the shader translator, self-contained (no .NET on the player's PC)
    toolset/wheels/       the recompiler's Python packages (lz4, capstone) for the bundled Python

On Linux (an old glibc host, so the result runs on newer ones: the release uses Ubuntu 22.04, glibc 2.35, below
SteamOS 3's; needs gcc, CMake, Ninja, Git, curl, the -dev packages listed in .github/workflows/ci.yml and a .NET 10
SDK) everything a Steam Deck lacks, since SteamOS ships no compiler and no development headers:
    toolset/llvm/         clang and lld (official LLVM release, trimmed to the compiler, linker and its headers)
    toolset/sysroot/      the host's glibc, kernel, libstdc++ and libgcc development files (exactly those packages)
    toolset/cmake/, toolset/bin/ninja
    toolset/sdl3/         SDL3, built (shared) with every video/audio backend it loads at run time
    toolset/vulkan/       Vulkan-Headers (the pinned version)
    toolset/bin/          spirv-val, ffmpeg + ffprobe (a minimal static build: WebM/VP9 to NV12)
    toolset/lib/          libopus.so.0
    toolset/shader-harness/ShaderHarness
    toolset/python/       a relocatable CPython (python-build-standalone) with lz4 and capstone installed

Not here: the Kraken decoder. Some files of the ooz revision it is built from carry no license notice of their own
(docs/BUILD.md), so a release does not redistribute it: the player's setup compiles it from the pinned source archive
with this toolset's compiler (tools/b3_win.py, deps).

Nothing here comes from the game. Every source is pinned (version, tag or commit) below or in tools/b3_win.py.
"""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_win  # noqa: E402  (the pins the setup verified the shader pipeline with: OOZ, RYUJINX, SPIRV_TOOLS, OPUS)

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = os.name == 'nt'
EXE = '.exe' if WINDOWS else ''

LLVM = ('https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/LLVM-23.1.5-Linux-X64.tar.xz', '23')
CMAKE_LINUX = 'https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-linux-x86_64.tar.gz'
NINJA_LINUX = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip'
SDL3 = ('https://github.com/libsdl-org/SDL.git', 'release-3.4.20')
FFMPEG = ('https://github.com/FFmpeg/FFmpeg.git', 'n8.1.3')
PYTHON_LINUX = ('https://github.com/astral-sh/python-build-standalone/releases/download/20261110/'
                'cpython-3.13.18+20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz')
# The Debian/Ubuntu packages whose files make the Linux sysroot: headers, crt objects and link libraries.
SYSROOT_PACKAGES = ['libc6-dev', 'libc6', 'linux-libc-dev', 'libstdc++-{gcc}-dev', 'libgcc-{gcc}-dev', 'libgcc-s1',
                    'libstdc++6']


def say(msg):
    print(f'toolset: {msg}', flush=True)


def run(cmd, cwd=None, env=None):
    say(' '.join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)


def download(url, dest):
    dest = Path(dest)
    if dest.exists():
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    say(f'downloading {url}')
    part = dest.with_suffix(dest.suffix + '.part')
    with urllib.request.urlopen(url) as r, part.open('wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    os.replace(part, dest)
    return dest


def git(url, ref, dest, sparse=None):
    if (dest / '.git').exists():
        return dest
    if sparse:
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', url, dest])
        run(['git', '-C', dest, 'sparse-checkout', 'init', '--cone'])
        run(['git', '-C', dest, 'sparse-checkout', 'set', sparse])
        run(['git', '-C', dest, 'checkout', '--detach', ref])
    elif len(ref) == 40:
        run(['git', 'clone', '--filter=blob:none', url, dest])
        run(['git', '-C', dest, 'checkout', '--detach', ref])
    else:
        run(['git', 'clone', '--depth', '1', '--branch', ref, url, dest])
    return dest


# ---- common to both: the shader translator, the Kraken decoder, spirv-val, libopus -----------------------------------

def shader_harness(work, out, jobs):
    """tools/bayonetta3_shaders/ShaderHarness.csproj over Ryujinx's shader sources (the pinned bayonetta3-mobile
    commit), published self-contained: one executable, no .NET install needed to run it."""
    ryu_root = git(b3_win.RYUJINX[0], b3_win.RYUJINX[1], work / 'bayonetta3-mobile', sparse=b3_win.RYUJINX[2])
    ryu = ryu_root / b3_win.RYUJINX[2]
    shader = ryu / 'src/Ryujinx.Graphics.Shader'
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1')
    run(['dotnet', 'build', shader / 'Ryujinx.Graphics.Shader.csproj', '-c', 'Release', '-p:RunAnalyzers=false'],
        env=env)
    lib = shader / 'bin/Release/net10.0'
    rid = 'win-x64' if WINDOWS else 'linux-x64'
    dest = out / 'shader-harness'
    run(['dotnet', 'publish', ROOT / 'tools/bayonetta3_shaders/ShaderHarness.csproj', '-c', 'Release', '-r', rid,
         '--self-contained', 'true', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
         '-p:InvariantGlobalization=true', f'-p:RyuShader={shader}', f'-p:RyuLib={lib}',
         f'-p:BaseIntermediateOutputPath={work / "harness-obj"}/', '-o', dest], env=env)
    built = dest / f'Ryujinx.Graphics.Shader{EXE}'
    final = dest / f'ShaderHarness{EXE}'
    if built.exists():
        os.replace(built, final)
    for extra in dest.glob('*.pdb'):
        extra.unlink()
    if not final.exists():
        raise SystemExit(f'no {final.name} in {dest}')
    for name in ('LICENSE.Ryujinx', 'LICENSE.SMW', 'LICENSE.bayonetta3-mobile'):
        shutil.copyfile(ROOT / 'tools/bayonetta3_shaders' / name, dest / name)


def spirv_val(work, out, jobs):
    src = git(*b3_win.SPIRV_TOOLS, work / 'SPIRV-Tools')
    run([sys.executable, src / 'utils/git-sync-deps'], cwd=src)
    build = work / 'spirv-build'
    flags = ['-DCMAKE_EXE_LINKER_FLAGS=-static'] if WINDOWS else \
        ['-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc']
    if WINDOWS:
        flags += [f'-DCMAKE_C_COMPILER={shutil.which("x86_64-w64-mingw32-clang")}',
                  f'-DCMAKE_CXX_COMPILER={shutil.which("x86_64-w64-mingw32-clang++")}']
    run(['cmake', '-S', src, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSPIRV_SKIP_TESTS=ON',
         '-DSPIRV_WERROR=OFF', *flags])
    run(['cmake', '--build', build, '--target', 'spirv-val', '--parallel', str(jobs)])
    shutil.copyfile(build / f'tools/spirv-val{EXE}', out / 'bin' / f'spirv-val{EXE}')
    if not WINDOWS:
        os.chmod(out / 'bin/spirv-val', 0o755)


def opus(work, out, jobs):
    url, digest = b3_win.OPUS
    tarball = download(url, work / Path(url).name)
    if b3_win.sha256(tarball) != digest:
        raise SystemExit(f'{tarball.name} does not match its pinned sha256')
    with tarfile.open(tarball) as tar:
        tar.extractall(work, filter='data')
    src = work / tarball.name[:-len('.tar.gz')]
    build = work / 'opus-build'
    flags = []
    if WINDOWS:
        flags = [f'-DCMAKE_C_COMPILER={shutil.which("x86_64-w64-mingw32-clang")}']
    run(['cmake', '-S', src, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DOPUS_BUILD_SHARED_LIBRARY=ON',
         '-DOPUS_BUILD_TESTING=OFF', '-DOPUS_BUILD_PROGRAMS=OFF', *flags])
    run(['cmake', '--build', build, '--parallel', str(jobs)])
    if WINDOWS:
        shutil.copyfile(build / 'libopus.dll', out / 'bin/libopus.dll')
    else:
        (out / 'lib').mkdir(exist_ok=True)
        real = max(build.glob('libopus.so.0.*'), key=lambda p: len(p.name))
        shutil.copyfile(real, out / 'lib/libopus.so.0')
    shutil.copyfile(src / 'COPYING', out / 'licenses/COPYING.opus')


def wheels(out):
    """The recompiler's Python packages as wheels, so the player's venv installs them without the network."""
    reqs = (ROOT / 'requirements-bayonetta3.txt').read_text(encoding='utf-8')
    pins = [line.strip() for line in reqs.splitlines() if line.strip() and not line.startswith('#')]
    pins = ['lz4==4.4.5' if p.startswith('lz4==') and WINDOWS else p for p in pins]
    run([sys.executable, '-m', 'pip', 'download', '--disable-pip-version-check', '--only-binary=:all:',
         '-d', out / 'wheels', *pins])


# ---- Linux only: what SteamOS lacks -----------------------------------------------------------------------------------

def llvm(work, out):
    """clang + lld from the official release, with only what compiling and linking C++ needs."""
    url, major = LLVM
    archive = download(url, work / Path(url).name)
    top = Path(url).name[:-len('.tar.xz')]
    keep = (f'{top}/bin/clang-{major}', f'{top}/bin/clang', f'{top}/bin/clang++', f'{top}/bin/lld',
            f'{top}/bin/ld.lld', f'{top}/bin/llvm-ar', f'{top}/bin/llvm-ranlib',
            f'{top}/lib/clang/{major}/include/', f'{top}/LICENSE.TXT')
    dest = out / 'llvm'
    with tarfile.open(archive) as tar:
        for member in tar:
            name = member.name
            if not any(name == k or (k.endswith('/') and name.startswith(k)) for k in keep):
                continue
            target = dest / Path(name).relative_to(top)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.issym():
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink() or target.exists():
                    target.unlink()
                target.symlink_to(member.linkname)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(member) as src, target.open('wb') as f:
                    shutil.copyfileobj(src, f, 1 << 20)
                os.chmod(target, member.mode | 0o644)
    for tool in ('clang', 'clang++', 'ld.lld'):
        if not (dest / 'bin' / tool).exists():
            raise SystemExit(f'LLVM archive had no bin/{tool}')
    # The release binaries carry their symbol tables: stripping saves about 100 MB of the download.
    for tool in (f'clang-{major}', 'lld', 'llvm-ar'):
        run(['strip', dest / 'bin' / tool])
    run([dest / 'bin/clang++', '--version'])


def sysroot(out):
    """Exactly the files of the host's C/C++ development packages, so clang --sysroot finds the same headers, crt
    objects and link libraries a full install would."""
    gcc = subprocess.check_output(['gcc', '-dumpversion'], text=True).strip().split('.')[0]
    dest = out / 'sysroot'
    count = 0
    for package in SYSROOT_PACKAGES:
        package = package.format(gcc=gcc)
        files = subprocess.check_output(['dpkg', '-L', package], text=True).splitlines()
        for f in files:
            p = Path(f)
            if not p.is_absolute() or p.is_dir():  # directories (and /lib, /lib64 links) come from their files
                continue
            if f.startswith(('/usr/share/', '/etc/', '/usr/bin/', '/usr/sbin/', '/sbin/', '/bin/', '/usr/libexec/')):
                continue
            # Merged /usr (/lib64 -> usr/lib64 on Ubuntu 22.04): store each file under its real folder; the links
            # between the folders are made below, as on the host.
            target = dest / os.path.realpath(p.parent).lstrip('/') / p.name
            target.parent.mkdir(parents=True, exist_ok=True)
            if p.is_symlink():
                if target.is_symlink() or target.exists():
                    target.unlink()
                link = os.readlink(p)
                if link.startswith('/'):  # a host path: point into the sysroot instead
                    link = os.path.relpath(dest / os.path.realpath(link).lstrip('/'), target.parent)
                target.symlink_to(link)
            elif p.is_file():
                shutil.copy2(p, target)
            count += 1
    # Ubuntu's /lib -> usr/lib and /lib64 layout: keep it, since libc.so's linker script names /lib/... paths.
    for name in ('lib', 'lib64'):
        if Path('/' + name).is_symlink() and not os.path.lexists(dest / name):
            (dest / name).symlink_to(os.readlink('/' + name))
    say(f'sysroot: {count} files from {", ".join(p.format(gcc=gcc) for p in SYSROOT_PACKAGES)} (gcc {gcc})')


def cmake_ninja(work, out):
    archive = download(CMAKE_LINUX, work / Path(CMAKE_LINUX).name)
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter='data')
    src = work / Path(CMAKE_LINUX).name[:-len('.tar.gz')]
    dest = out / 'cmake'
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns('doc', 'man', 'Help', 'cmake-gui', 'ccmake'))
    import zipfile
    with zipfile.ZipFile(download(NINJA_LINUX, work / 'ninja-linux.zip')) as z:
        z.extract('ninja', out / 'bin')
    os.chmod(out / 'bin/ninja', 0o755)


def sdl3(work, out, jobs):
    src = git(*SDL3, work / 'SDL')
    build = work / 'sdl-build'
    run(['cmake', '-S', src, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSDL_SHARED=ON',
         '-DSDL_STATIC=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', '-DSDL_INSTALL_DOCS=OFF',
         f'-DCMAKE_INSTALL_PREFIX={out / "sdl3"}', '-DCMAKE_INSTALL_LIBDIR=lib'])
    run(['cmake', '--build', build, '--parallel', str(jobs)])
    run(['cmake', '--install', build])
    shutil.copyfile(src / 'LICENSE.txt', out / 'licenses/LICENSE.SDL3')


def ffmpeg(work, out, jobs):
    """ffmpeg and ffprobe with only what runtime/src/movie.cpp asks of them: Matroska/WebM in, VP9 decoded, scaled to
    NV12, raw video out. Static, LGPL (no --enable-gpl), a few MB."""
    src = git(*FFMPEG, work / 'FFmpeg')
    build = work / 'ffmpeg-build'
    build.mkdir(exist_ok=True)
    run([src / 'configure', '--prefix=' + str(work / 'ffmpeg-install'), '--disable-everything', '--disable-doc',
         '--disable-network', '--disable-autodetect', '--disable-shared', '--enable-static', '--disable-debug',
         '--enable-ffmpeg', '--enable-ffprobe', '--enable-avcodec', '--enable-avformat', '--enable-avfilter',
         '--enable-swscale', '--enable-decoder=vp9,opus,vorbis,rawvideo', '--enable-parser=vp9',
         '--enable-demuxer=matroska', '--enable-encoder=rawvideo', '--enable-muxer=rawvideo',
         '--enable-protocol=file,pipe', '--enable-filter=scale,format,null,anull,buffer,buffersink,abuffer,'
         'abuffersink,aformat,setpts,asetpts,trim,atrim,fps', '--extra-ldflags=-static-libgcc'], cwd=build)
    run(['make', f'-j{jobs}'], cwd=build)
    for tool in ('ffmpeg', 'ffprobe'):
        shutil.copyfile(build / tool, out / 'bin' / tool)
        os.chmod(out / 'bin' / tool, 0o755)
    shutil.copyfile(src / 'COPYING.LGPLv2.1', out / 'licenses/COPYING.ffmpeg')


def vulkan_headers(work, out):
    src = git(*b3_win.VULKAN_HEADERS, work / 'Vulkan-Headers')
    dest = out / 'vulkan'
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src / 'include', dest / 'include')
    shutil.copyfile(src / 'LICENSE.md', out / 'licenses/LICENSE.Vulkan-Headers')


def python_linux(work, out):
    archive = download(PYTHON_LINUX, work / Path(PYTHON_LINUX).name.replace('+', '_'))
    dest = out / 'python'
    if dest.exists():
        shutil.rmtree(dest)
    with tarfile.open(archive) as tar:
        tar.extractall(work / 'py', filter='tar')  # 'tar': the archive's own symlinks (bin/python3 -> python3.13)
    shutil.move(str(work / 'py/python'), dest)
    reqs = (ROOT / 'requirements-bayonetta3.txt').read_text(encoding='utf-8')
    pins = [line.strip() for line in reqs.splitlines() if line.strip() and not line.startswith('#')]
    run([dest / 'bin/python3', '-m', 'pip', 'install', '--disable-pip-version-check', '--only-binary=:all:', *pins])
    run([dest / 'bin/python3', '-c', 'import lz4.block, capstone, tkinter; print("python ok")'])
    # The player runs scripts with it, never pip, IDLE or the test suite.
    for sub in ('lib/python3.13/site-packages/pip', 'lib/python3.13/idlelib', 'lib/python3.13/ensurepip',
                'lib/python3.13/test', 'lib/python3.13/config-3.13-x86_64-linux-gnu', 'include', 'share'):
        shutil.rmtree(dest / sub, ignore_errors=True)
    for path in dest.rglob('__pycache__'):
        shutil.rmtree(path, ignore_errors=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--out', default=str(ROOT / 'toolset'))
    p.add_argument('--work', default=str(ROOT / 'private/toolset-work'))
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    p.add_argument('--only', help='comma-separated parts (harness,spirv,opus,wheels,vulkan,llvm,sysroot,cmake,sdl3,'
                   'ffmpeg,vulkan,python)')
    a = p.parse_args()
    out, work = Path(a.out).resolve(), Path(a.work).resolve()
    for d in (out / 'bin', out / 'licenses', work):
        d.mkdir(parents=True, exist_ok=True)
    if platform.machine().lower() not in ('amd64', 'x86_64'):
        raise SystemExit('the release toolset is built on x86-64')
    parts = {
        'harness': lambda: shader_harness(work, out, a.jobs),
        'spirv': lambda: spirv_val(work, out, a.jobs),
        'opus': lambda: opus(work, out, a.jobs),
    }
    parts['vulkan'] = lambda: vulkan_headers(work, out)
    if WINDOWS:
        parts['wheels'] = lambda: wheels(out)
    else:
        parts.update({
            'llvm': lambda: llvm(work, out),
            'sysroot': lambda: sysroot(out),
            'cmake': lambda: cmake_ninja(work, out),
            'sdl3': lambda: sdl3(work, out, a.jobs),
            'ffmpeg': lambda: ffmpeg(work, out, a.jobs),
            'python': lambda: python_linux(work, out),
        })
    chosen = a.only.split(',') if a.only else list(parts)
    for name in chosen:
        say(f'--- {name}')
        parts[name]()
    (out / 'TOOLSET.txt').write_text(
        'Built by tools/build_toolset.py from pinned sources (see that file and tools/b3_win.py). '
        'No game files, code or data.\n', encoding='utf-8')
    say(f'done: {out}')


if __name__ == '__main__':
    main()
