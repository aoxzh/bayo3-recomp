#!/usr/bin/env python3
"""The Android setup toolset: what the APK carries so the phone turns the player's own dump into the game by itself,
with no PC. Built on a Linux x86-64 host (CI) with the Android NDK; nothing in it comes from the game.

    python3 tools/build_android_toolset.py --ndk <android-ndk-r29> --out android-toolset [--work DIR] [--jobs N]

android-toolset/lib/       executables and libraries for the APK's lib/arm64-v8a (Android runs only those, W^X):
    libclang.so, liblld.so      clang and lld 23 built to run on Android arm64 (clang++/ld.lld via links: b3_win.py)
    libshaderharness.so         the shader translator (Ryujinx's, NativeAOT for linux-bionic-arm64: no .NET runtime)
    libspirvval.so              spirv-val
    libb3python.so              python (CPython's official Android build is embed-only: android/ondevice/pyexe.c)
    libpython3.14.so ...        CPython and the libraries its extension modules need
    libb3dlmain.so              runs a program compiled on the phone as a shared library (android/ondevice/dlmain.c)
    libb3loader.so              the NativeActivity: loads the game the setup built (android/ondevice/loader.c)
    libopus.so                  the game's Opus voices
android-toolset/files/     unpacked into the app's files by the setup (assets/ondevice.zip, with this repository's
                           tools, recompiler, runtime headers and config):
    toolset/sysroot/            the NDK's sysroot for arm64, API 29 (headers, crt objects, libc++)
    toolset/clang/              clang's resource folder (its headers, compiler-rt builtins, libunwind)
    toolset/runtime/            libbayonetta3.a: the runtime alone (CMake NX_ANDROID_RUNTIME_ARCHIVE)
    toolset/include/            sse2neon.h (MIT, its license in the header): the Kraken decoder's SSE on ARM
    python/                     CPython's standard library, capstone (Python part + libcapstone.so) and a pure lz4

Not here: the Kraken decoder (its license keeps it out of a release); the phone compiles it from the pinned source.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_win  # noqa: E402  (pins: RYUJINX, SPIRV_TOOLS, OPUS, VULKAN_HEADERS)
import build_toolset  # noqa: E402  (download, git, the harness sources)

ROOT = Path(__file__).resolve().parents[1]
API = 29
LLVM_SRC = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/llvm-project-23.1.5.src.tar.xz'
LLVM_MAJOR = '23'
PYTHON = ('https://www.python.org/ftp/python/3.14.10/python-3.14.10-aarch64-linux-android.tar.gz',
          '423c80b957330cf0ebb92b9b20f2294c292144a441bdb0ceb8c69d14928e569b', '3.14')
CAPSTONE = ('https://github.com/capstone-engine/capstone.git', '5.0.6')
SSE2NEON = ('https://raw.githubusercontent.com/DLTcollab/sse2neon/v1.8.0/sse2neon.h',
            '07723c9f9457dd4316f1fde3dd4eb6f31dd67d9955f6c21f4e609ac1698be48a')
say = build_toolset.say
run = build_toolset.run


def ndk_paths(ndk):
    prebuilt = ndk / 'toolchains/llvm/prebuilt/linux-x86_64'
    return {
        'toolchain': ndk / 'build/cmake/android.toolchain.cmake',
        'cc': prebuilt / 'bin' / f'aarch64-linux-android{API}-clang',
        'cxx': prebuilt / 'bin' / f'aarch64-linux-android{API}-clang++',
        'strip': prebuilt / 'bin/llvm-strip',
        'sysroot': prebuilt / 'sysroot',
        'resource': next((prebuilt / 'lib/clang').iterdir()),
    }


def cmake_android(src, build, ndk, extra, jobs, targets):
    n = ndk_paths(ndk)
    run(['cmake', '-G', 'Ninja', '-S', src, '-B', build, f'-DCMAKE_TOOLCHAIN_FILE={n["toolchain"]}',
         '-DANDROID_ABI=arm64-v8a', f'-DANDROID_PLATFORM=android-{API}', '-DANDROID_STL=c++_static',
         '-DCMAKE_BUILD_TYPE=Release', *extra])
    run(['ninja', '-C', build, f'-j{jobs}', *targets])


def strip_to(ndk, src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    run([ndk_paths(ndk)['strip'], '--strip-unneeded', '-o', dest, src])


# ---- clang and lld that run on the phone --------------------------------------------------------------------------

def llvm(work, out, ndk, jobs):
    archive = build_toolset.download(LLVM_SRC, work / Path(LLVM_SRC).name)
    src = work / Path(LLVM_SRC).name[:-len('.tar.xz')]
    if not src.is_dir():
        with tarfile.open(archive) as tar:
            tar.extractall(work, filter='data')
    build = work / 'llvm-android'
    cmake_android(src / 'llvm', build, ndk, [
        '-DLLVM_ENABLE_PROJECTS=clang;lld', '-DLLVM_TARGETS_TO_BUILD=AArch64',
        f'-DLLVM_HOST_TRIPLE=aarch64-linux-android{API}', f'-DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-linux-android{API}',
        '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_TERMINFO=OFF',
        '-DLLVM_ENABLE_LIBEDIT=OFF', '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
        '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_DOCS=OFF', '-DCLANG_ENABLE_ARCMT=OFF',
        '-DCLANG_ENABLE_STATIC_ANALYZER=OFF', '-DCLANG_PLUGIN_SUPPORT=OFF', '-DLLVM_ENABLE_PLUGINS=OFF',
        '-DLLVM_PARALLEL_LINK_JOBS=1',  # the NDK links with its lld already
        '-DCROSS_TOOLCHAIN_FLAGS_NATIVE=-DCMAKE_C_COMPILER=gcc;-DCMAKE_CXX_COMPILER=g++'],
        jobs, ['clang', 'lld', 'clang-resource-headers'])
    strip_to(ndk, build / f'bin/clang-{LLVM_MAJOR}', out / 'lib/libclang.so')
    strip_to(ndk, build / 'bin/lld', out / 'lib/liblld.so')
    # The resource folder: this clang's own headers, and the NDK's compiler-rt builtins and libunwind for arm64.
    resource = out / 'files/toolset/clang'
    shutil.rmtree(resource, ignore_errors=True)
    shutil.copytree(build / 'lib/clang' / LLVM_MAJOR / 'include', resource / 'include')
    ndk_rt = ndk_paths(ndk)['resource'] / 'lib/linux'
    (resource / 'lib/linux/aarch64').mkdir(parents=True)
    shutil.copyfile(ndk_rt / 'libclang_rt.builtins-aarch64-android.a',
                    resource / 'lib/linux/libclang_rt.builtins-aarch64-android.a')
    shutil.copyfile(ndk_rt / 'aarch64/libunwind.a', resource / 'lib/linux/aarch64/libunwind.a')
    shutil.copyfile(src / 'llvm/LICENSE.TXT', out / 'files/licenses/LICENSE.llvm')


def sysroot(out, ndk):
    """The NDK's sysroot, only what an arm64 API 29 build reads: every header (less other ABIs' asm folders), the
    static C++ libraries and the API 29 crt objects and stubs."""
    src = ndk_paths(ndk)['sysroot']
    dest = out / 'files/toolset/sysroot'
    shutil.rmtree(dest, ignore_errors=True)
    other = {'arm-linux-androideabi', 'i686-linux-android', 'x86_64-linux-android', 'riscv64-linux-android'}
    shutil.copytree(src / 'usr/include', dest / 'usr/include', symlinks=True,
                    ignore=lambda d, names: [n for n in names if Path(d) == src / 'usr/include' and n in other])
    lib = src / 'usr/lib/aarch64-linux-android'
    (dest / 'usr/lib/aarch64-linux-android').mkdir(parents=True)
    for name in ('libc++_static.a', 'libc++abi.a', 'libc++.a', 'libunwind.a', 'libatomic.a', 'libcompiler_rt-extras.a'):
        if (lib / name).exists():
            shutil.copy2(lib / name, dest / 'usr/lib/aarch64-linux-android' / name)
    shutil.copytree(lib / str(API), dest / 'usr/lib/aarch64-linux-android' / str(API), symlinks=True)
    notice = ndk / 'NOTICE'
    if notice.exists():
        shutil.copyfile(notice, out / 'files/licenses/NOTICE.android-ndk')


# ---- the runtime, the small executables, libopus, spirv-val --------------------------------------------------------

def runtime(work, out, ndk, jobs, vulkan_include):
    build = work / 'runtime-android'
    shutil.rmtree(build, ignore_errors=True)
    cmake_android(ROOT, build, ndk, [
        '-DCMAKE_C_FLAGS=-march=armv8.2-a+crypto', '-DCMAKE_CXX_FLAGS=-march=armv8.2-a+crypto',
        '-DNX_GAME=bayonetta3', '-DNX_ANDROID_RUNTIME_ARCHIVE=ON', f'-DNX_GEN_DIR={work / "no-generated-code"}',
        f'-DNX_VULKAN_INCLUDE={vulkan_include}'], jobs, ['bayonetta3'])
    dest = out / 'files/toolset/runtime'
    dest.mkdir(parents=True, exist_ok=True)
    run([ndk_paths(ndk)['strip'], '--strip-debug', '-o', dest / 'libbayonetta3.a', build / 'libbayonetta3.a'])


def small_programs(out, ndk, python_prefix):
    n = ndk_paths(ndk)
    lib = out / 'lib'
    lib.mkdir(parents=True, exist_ok=True)
    od = ROOT / 'android/ondevice'
    run([n['cc'], '-O2', '-shared', '-fPIC', od / 'loader.c', '-landroid', '-llog', '-o', lib / 'libb3loader.so'])
    run([n['cc'], '-O2', '-fPIE', '-pie', od / 'dlmain.c', '-o', lib / 'libb3dlmain.so'])
    run([n['cc'], '-O2', '-fPIE', '-pie', f'-I{python_prefix / "include" / ("python" + PYTHON[2])}', od / 'pyexe.c',
         f'-L{python_prefix / "lib"}', f'-lpython{PYTHON[2]}', '-o', lib / 'libb3python.so'])


def opus(work, out, ndk, jobs):
    url, digest = b3_win.OPUS
    tarball = build_toolset.download(url, work / Path(url).name)
    if b3_win.sha256(tarball) != digest:
        raise SystemExit(f'{tarball.name} does not match its pinned sha256')
    src = work / tarball.name[:-len('.tar.gz')]
    if not src.is_dir():
        with tarfile.open(tarball) as tar:
            tar.extractall(work, filter='data')
    build = work / 'opus-android'
    cmake_android(src, build, ndk, ['-DOPUS_BUILD_SHARED_LIBRARY=ON', '-DOPUS_BUILD_TESTING=OFF',
                                    '-DOPUS_BUILD_PROGRAMS=OFF'], jobs, [])
    strip_to(ndk, build / 'libopus.so', out / 'lib/libopus.so')
    shutil.copyfile(src / 'COPYING', out / 'files/licenses/COPYING.opus')


def spirv_val(work, out, ndk, jobs):
    src = build_toolset.git(*b3_win.SPIRV_TOOLS, work / 'SPIRV-Tools')
    run([sys.executable, src / 'utils/git-sync-deps'], cwd=src)
    build = work / 'spirv-android'
    # SPIRV-Tools builds no executables for Android (its CMakeLists forces SPIRV_SKIP_EXECUTABLES): the library with
    # CMake, spirv-val's four sources against it here.
    cmake_android(src, build, ndk, ['-DSPIRV_SKIP_TESTS=ON', '-DSPIRV_WERROR=OFF'], jobs, ['SPIRV-Tools-static'])
    t = src / 'tools'
    run([ndk_paths(ndk)['cxx'], '-std=c++17', '-O2', '-fPIE', '-pie', f'-I{src}', f'-I{src / "include"}', f'-I{build}',
         t / 'util/flags.cpp', t / 'val/val.cpp', t / 'util/cli_consumer.cpp', t / 'io.cpp',
         build / 'source/libSPIRV-Tools.a', '-static-libstdc++', '-o', build / 'spirv-val'])
    strip_to(ndk, build / 'spirv-val', out / 'lib/libspirvval.so')
    shutil.copyfile(src / 'LICENSE', out / 'files/licenses/LICENSE.SPIRV-Tools')


def headers(work, out):
    dest = out / 'files/toolset/include/sse2neon.h'
    dest.parent.mkdir(parents=True, exist_ok=True)
    build_toolset.download(SSE2NEON[0], dest)
    if b3_win.sha256(dest) != SSE2NEON[1]:
        dest.unlink()
        raise SystemExit('sse2neon.h does not match its pinned sha256')
    # The Vulkan headers build the runtime archive here; the phone compiles only generated code, which needs none.
    return build_toolset.git(*b3_win.VULKAN_HEADERS, work / 'Vulkan-Headers') / 'include'


# ---- Python ----------------------------------------------------------------------------------------------------------

def python(work, out, ndk, jobs):
    url, digest, version = PYTHON
    archive = build_toolset.download(url, work / Path(url).name)
    if b3_win.sha256(archive) != digest:
        raise SystemExit(f'{archive.name} does not match its pinned sha256')
    top = work / 'python-android'
    if not (top / 'prefix').is_dir():
        top.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(top, filter='data')
    prefix = top / 'prefix'
    for name in (f'libpython{version}.so', 'libcrypto_python.so', 'libssl_python.so', 'libsqlite3_python.so'):
        if (prefix / 'lib' / name).exists():
            strip_to(ndk, prefix / 'lib' / name, out / 'lib' / name)
    stdlib = out / 'files/python/lib' / f'python{version}'
    shutil.rmtree(stdlib, ignore_errors=True)
    skip = {'test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', '__pycache__', 'site-packages'}
    shutil.copytree(prefix / 'lib' / f'python{version}', stdlib,
                    ignore=lambda d, names: [n for n in names if n in skip])
    site = stdlib / 'site-packages'
    site.mkdir()
    shutil.copytree(ROOT / 'android/ondevice/python/lz4', site / 'lz4')
    # capstone: its Python part from the tagged source, its C library built with the NDK.
    src = build_toolset.git(*CAPSTONE, work / 'capstone')
    build = work / 'capstone-android'
    shutil.rmtree(build, ignore_errors=True)
    cmake_android(src, build, ndk, ['-DBUILD_SHARED_LIBS=ON', '-DCAPSTONE_BUILD_SHARED=ON','-DCAPSTONE_BUILD_TESTS=OFF',
                                    '-DCAPSTONE_BUILD_CSTOOL=OFF', '-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF',
                                    '-DCAPSTONE_ARM64_SUPPORT=ON'], jobs, [])
    pkg = site / 'capstone'
    shutil.copytree(src / 'bindings/python/capstone', pkg, ignore=shutil.ignore_patterns('__pycache__'))
    so = next(p for p in build.glob('libcapstone.so*') if not p.is_symlink())
    strip_to(ndk, so, pkg / 'lib/libcapstone.so')
    shutil.copyfile(src / 'LICENSE.TXT', out / 'files/licenses/LICENSE.capstone')
    shutil.copyfile(prefix.parent / 'README.md' if (prefix.parent / 'README.md').exists() else stdlib / 'LICENSE.txt',
                    out / 'files/licenses/README.python-android')
    return prefix


# ---- the shader translator, ahead-of-time compiled for Android ------------------------------------------------------

def harness(work, out, ndk):
    ryu_root = build_toolset.git(b3_win.RYUJINX[0], b3_win.RYUJINX[1], work / 'bayonetta3-mobile',
                                 sparse=b3_win.RYUJINX[2])
    shader = ryu_root / b3_win.RYUJINX[2] / 'src/Ryujinx.Graphics.Shader'
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1')
    run(['dotnet', 'build', shader / 'Ryujinx.Graphics.Shader.csproj', '-c', 'Release', '-p:RunAnalyzers=false'],
        env=env)
    # NativeAOT for linux-bionic-arm64 links with the NDK's clang, found first on PATH.
    env['PATH'] = f'{ndk_paths(ndk)["cc"].parent}{os.pathsep}{env["PATH"]}'
    dest = work / 'harness-android'
    run(['dotnet', 'publish', ROOT / 'tools/bayonetta3_shaders/ShaderHarness.csproj', '-c', 'Release',
         '-r', 'linux-bionic-arm64', '-p:PublishAot=true', '-p:DisableUnsupportedError=true',
         '-p:PublishAotUsingRuntimePack=true', '-p:InvariantGlobalization=true', '-p:StripSymbols=true',
         f'-p:RyuShader={shader}', f'-p:RyuLib={shader / "bin/Release/net10.0"}',
         f'-p:BaseIntermediateOutputPath={work / "harness-android-obj"}/', '-o', dest], env=env)
    built = dest / 'Ryujinx.Graphics.Shader'
    if not built.exists():
        raise SystemExit(f'no translator in {dest}')
    shutil.copyfile(built, out / 'lib/libshaderharness.so')
    for name in ('LICENSE.Ryujinx', 'LICENSE.SMW', 'LICENSE.bayonetta3-mobile'):
        shutil.copyfile(ROOT / 'tools/bayonetta3_shaders' / name, out / 'files/licenses' / name)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--ndk', required=True, help='an Android NDK r29 for linux-x86_64')
    p.add_argument('--out', default=str(ROOT / 'android-toolset'))
    p.add_argument('--work', default=str(ROOT / 'private/android-toolset-work'))
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    p.add_argument('--only', help='comma-separated parts (headers,sysroot,llvm,runtime,opus,spirv,python,small,harness)')
    a = p.parse_args()
    ndk, out, work = Path(a.ndk).resolve(), Path(a.out).resolve(), Path(a.work).resolve()
    for d in (out / 'lib', out / 'files/licenses', work):
        d.mkdir(parents=True, exist_ok=True)
    vulkan = work / 'Vulkan-Headers/include'
    prefix = work / 'python-android/prefix'
    parts = {
        'headers': lambda: headers(work, out),
        'sysroot': lambda: sysroot(out, ndk),
        'llvm': lambda: llvm(work, out, ndk, a.jobs),
        'runtime': lambda: runtime(work, out, ndk, a.jobs, vulkan),
        'opus': lambda: opus(work, out, ndk, a.jobs),
        'spirv': lambda: spirv_val(work, out, ndk, a.jobs),
        'python': lambda: python(work, out, ndk, a.jobs),
        'small': lambda: small_programs(out, ndk, prefix),
        'harness': lambda: harness(work, out, ndk),
    }
    for name in (a.only.split(',') if a.only else list(parts)):
        say(f'--- {name}')
        parts[name]()
    (out / 'files/toolset/TOOLSET.txt').write_text(
        'Built by tools/build_android_toolset.py from pinned sources. No game files, code or data.\n',
        encoding='utf-8')
    say(f'done: {out}')


if __name__ == '__main__':
    main()
