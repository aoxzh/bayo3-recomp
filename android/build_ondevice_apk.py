#!/usr/bin/env python3
"""The release APK: the launcher and the setup tools, no game code. The player puts their own dump in Internal
storage/Bayonetta 3/dump and the phone builds the game itself (games/bayonetta3/java/.../Setup.java).

    python3 android/build_ondevice_apk.py --toolset android-toolset --sdk <android-sdk> [--out dist]
        [--version v0.3.0] [--keystore release.jks --ks-pass env:B3_KS_PASS]

--toolset is tools/build_android_toolset.py's output. Without --keystore the APK is signed with a development key
made in private/ (an APK signed with another key does not install over it: uninstall first). Runs on Linux (CI) and
anywhere the SDK's build-tools and a JDK (javac) run.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
# runtime/: its headers for the generated code, and runtime/src for the shader check (bayonetta3_shaders.py verify).
PACKAGE_DIRS = ('tools', 'recomp', 'config', 'runtime', 'games/bayonetta3')
SKIP = re.compile(r'\.(jpe?g|png|ttf|ps1|cmd|bat)$|^tools/launcher_assets/|/java/|^tools/bayonetta3_shaders/.*\.cs$')


def run(cmd, **kw):
    print('apk:', ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def build_tools(sdk):
    bt = sorted((sdk / 'build-tools').iterdir(), key=lambda p: [int(x) for x in re.findall(r'\d+', p.name)])
    for d in reversed(bt):
        if all((d / t).exists() or (d / f'{t}.bat').exists() or (d / f'{t}.exe').exists()
               for t in ('aapt2', 'zipalign', 'apksigner', 'd8')):
            return d
    raise SystemExit(f'no complete build-tools in {sdk}')


def tool(bt, name):
    for n in (name, name + '.exe', name + '.bat'):
        if (bt / n).exists():
            return bt / n
    raise SystemExit(f'{name} not in {bt}')


def platform_jar(sdk):
    jars = sorted((sdk / 'platforms').glob('android-*/android.jar'),
                  key=lambda p: int(re.sub(r'\D', '', p.parent.name) or 0))
    if not jars:
        raise SystemExit(f'no platform in {sdk}/platforms')
    return jars[-1]


def tracked(prefixes):
    out = subprocess.run(['git', '-C', ROOT, 'ls-files', '-z', '--', *prefixes], check=True,
                         capture_output=True).stdout.decode().split('\0')
    return [p for p in out if p and not SKIP.search(p) and (ROOT / p).is_file()]


def ondevice_zip(toolset, dest):
    """What Setup.java unpacks into <files>/b3: repo/ (this project's tools, as tracked), toolset/, python/, licenses/."""
    n = 0
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in tracked(PACKAGE_DIRS) + ['requirements-bayonetta3.txt', 'LICENSE']:
            if (ROOT / rel).is_file():
                z.write(ROOT / rel, f'repo/{rel}')
                n += 1
        for top in ('toolset', 'python', 'licenses'):
            base = toolset / 'files' / top
            for d, _, names in os.walk(base, followlinks=True):
                for name in sorted(names):
                    p = Path(d) / name
                    if '__pycache__' in p.parts:
                        continue
                    z.write(p, f'{top}/{p.relative_to(base).as_posix()}')
                    n += 1
    print(f'apk: {dest.name}: {n} files, {dest.stat().st_size / 1e6:.0f} MB', flush=True)


def manifest(dest, version, code, test_package=None):
    text = (ROOT / 'games/bayonetta3/AndroidManifest.xml').read_text(encoding='utf-8')
    text = text.replace('android:value="bayonetta3"', 'android:value="b3loader"')  # the game is built on the phone
    if test_package:  # beside another install of the app: its own authority, debuggable for run-as
        text = text.replace('io.github.bayonetta3recomp.support', f'{test_package}.support')
        text = text.replace('Bayonetta 3 Recomp (Development)', 'B3 on-device test')
    else:
        text = text.replace('Bayonetta 3 Recomp (Development)', 'Bayonetta 3 Recomp')
        text = text.replace('android:debuggable="true"', 'android:debuggable="false"')
    text = re.sub(r'android:versionCode="\d+"', f'android:versionCode="{code}"', text)
    text = re.sub(r'android:versionName="[^"]*"', f'android:versionName="{version}"', text)
    if 'b3loader' not in text:
        raise SystemExit('the manifest no longer names the game library as android:value="bayonetta3"')
    dest.write_text(text, encoding='utf-8')


def version_code(version):
    """v0.3.0 -> 3000 (major*1e6 + minor*1e3 + patch, at least 2): later releases install over earlier ones."""
    m = re.match(r'v?(\d+)\.(\d+)\.(\d+)', version)
    return max(2, int(m[1]) * 1000000 + int(m[2]) * 1000 + int(m[3])) if m else 2


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--toolset', required=True, help="tools/build_android_toolset.py's output")
    p.add_argument('--sdk', default=os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT'))
    p.add_argument('--out', default=str(ROOT / 'dist'))
    p.add_argument('--version', help='default: git describe')
    p.add_argument('--keystore', help='a release keystore (default: a development key in private/)')
    p.add_argument('--ks-pass', default='pass:android', help="apksigner's --ks-pass (e.g. env:B3_KS_PASS)")
    p.add_argument('--ks-alias', default='b3')
    p.add_argument('--test-package', help='an application id for a test install beside the real app (debuggable)')
    a = p.parse_args()
    if not a.sdk:
        raise SystemExit('--sdk (or ANDROID_HOME) is needed')
    sdk, toolset, out = Path(a.sdk), Path(a.toolset).resolve(), Path(a.out).resolve()
    version = a.version or subprocess.run(['git', '-C', ROOT, 'describe', '--tags', '--always'], check=True,
                                          capture_output=True, text=True).stdout.strip()
    bt, jar = build_tools(sdk), platform_jar(sdk)
    stage = ROOT / 'private/ondevice-apk'
    shutil.rmtree(stage, ignore_errors=True)
    for d in ('lib/arm64-v8a', 'assets/launcher', 'classes', 'dex'):
        (stage / d).mkdir(parents=True)

    libs = sorted((toolset / 'lib').glob('lib*.so'))
    need = {'libclang.so', 'liblld.so', 'libb3python.so', 'libpython3.14.so', 'libshaderharness.so', 'libspirvval.so',
            'libb3dlmain.so', 'libb3loader.so', 'libopus.so'}
    missing = need - {x.name for x in libs}
    if missing:
        raise SystemExit(f'the toolset lacks {", ".join(sorted(missing))}')
    if any(x.name == 'libbayonetta3.so' for x in libs):
        raise SystemExit('libbayonetta3.so is game code: it never goes into a release APK')
    for x in libs:
        shutil.copyfile(x, stage / 'lib/arm64-v8a' / x.name)
    shutil.copyfile(ROOT / 'tools/launcher_assets/background.jpg', stage / 'assets/launcher/background.jpg')
    ondevice_zip(toolset, stage / 'assets/ondevice.zip')

    manifest(stage / 'AndroidManifest.xml', version, version_code(version), a.test_package)
    rename = ['--rename-manifest-package', a.test_package] if a.test_package else []
    run([tool(bt, 'aapt2'), 'link', '-o', stage / 'base.apk', '-I', jar, '--manifest', stage / 'AndroidManifest.xml',
         '--min-sdk-version', '29', '--target-sdk-version', '35', '-A', stage / 'assets', '-0', 'zip', *rename])
    sources = sorted(str(s) for s in (ROOT / 'games/bayonetta3/java').rglob('*.java'))
    run(['javac', '--release', '8', '-Xlint:-options', '-encoding', 'UTF-8', '-classpath', jar, '-d', stage / 'classes',
         *sources])
    classes = sorted(str(c) for c in (stage / 'classes').rglob('*.class'))
    run([tool(bt, 'd8'), '--release', '--min-api', '29', '--lib', jar, '--output', stage / 'dex', *classes])
    # A new zip with aapt2's entries as they were (ondevice.zip stays stored) and the code: appending to aapt2's zip
    # leaves local headers zipalign reports as mismatched.
    with zipfile.ZipFile(stage / 'base.apk') as src, \
            zipfile.ZipFile(stage / 'full.apk', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for info in src.infolist():
            z.writestr(zipfile.ZipInfo(info.filename, info.date_time), src.read(info), compress_type=info.compress_type)
        z.write(stage / 'dex/classes.dex', 'classes.dex')
        for x in sorted((stage / 'lib/arm64-v8a').iterdir()):
            z.write(x, f'lib/arm64-v8a/{x.name}')  # compressed: the manifest's extractNativeLibs unpacks them
    run([tool(bt, 'zipalign'), '-f', '4', stage / 'full.apk', stage / 'aligned.apk'])

    keystore, ks_pass, alias = a.keystore, a.ks_pass, a.ks_alias
    if not keystore:
        keystore = ROOT / 'private/build-android/signing/ondevice-dev.jks'
        if not keystore.exists():
            keystore.parent.mkdir(parents=True, exist_ok=True)
            run(['keytool', '-genkeypair', '-noprompt', '-keystore', keystore, '-storetype', 'PKCS12', '-alias', alias,
                 '-storepass', 'android', '-keypass', 'android', '-keyalg', 'RSA', '-keysize', '3072',
                 '-validity', '10000', '-dname', 'CN=B3 Development'])
        ks_pass = 'pass:android'
    out.mkdir(parents=True, exist_ok=True)
    apk = out / f'Bayonetta3Recomp-{version}-android-arm64.apk'
    run([tool(bt, 'apksigner'), 'sign', '--ks', keystore, '--ks-key-alias', alias, '--ks-pass', ks_pass,
         '--out', apk, stage / 'aligned.apk'])
    run([tool(bt, 'apksigner'), 'verify', apk])
    with zipfile.ZipFile(apk) as z:  # the release rule, checked on the result
        bad = [n for n in z.namelist() if n.endswith('libbayonetta3.so') or n.startswith('assets/spirv')]
    if bad:
        apk.unlink()
        raise SystemExit(f'game-derived files in the APK: {bad}')
    print(f'apk: {apk} ({apk.stat().st_size / 1e6:.0f} MB)', flush=True)


if __name__ == '__main__':
    sys.exit(main())
