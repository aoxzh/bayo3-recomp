# Playing Bayonetta 3

The game already runs and is playable on Windows, Linux/Steam Deck and Android, with performance measurements for each platform. macOS also has recorded gameplay and performance runs, though its older diagnostic runs do not validate every step of the normal visible-window recipe below. See [the README](../README.md) for current status. These recipes were checked against the launch scripts and runtime, and project reports informed them; this documentation pass did not repeat a clean rebuild and full play session on every platform.

## Prepare once

Run commands from the repository root and replace `/path/to/...` with your own locations. You need a matching, legally obtained copy of Bayonetta 3 1.2.0: its ExeFS (`main`, `subsdk0`, `sdk`, `main.npdm`), its RomFS, generated CPU code, and translated game shaders. First boot does not require a downloaded save. Choosing chapters that are already unlocked, such as Chapter 8, does require a progressed save of your own.

[BUILD.md](BUILD.md) covers the Python environment, the identity-checked CPU generation, and the shader pipeline with its external decoder, Ryujinx and .NET dependencies. Preparation runs in Linux, macOS or native Windows Python with the pinned requirements; on Windows the links it creates are directory junctions. A complete generation has been run on native Windows for both programs. Generating on a POSIX host and transferring the private generated tree and game inputs to Windows, keeping the directory layout and repairing links for that host, is the tested route. Keep generated files and game inputs in `private/` or another local-only location.

Once shader extraction, translation and verification have succeeded, publish the same work directory to the runtime bundle used throughout this guide:

```bash
python tools/bayonetta3_shaders.py publish \\
  --work "$PWD/private/shader-work-local" \\
  --publish-dir "$PWD/private/bayonetta3-shaders-repro"
```

Use the Python environment from BUILD. `publish` refuses an existing destination, so either keep an existing bundle or choose a fresh destination and update the launch path. Nothing is uploaded; this only copies files locally.

Expected inputs:

```text
private/game/ExeFS/{main,subsdk0,sdk,main.npdm}
private/game/RomFS/                       # full extracted RomFS
private/bayonetta3/gen/{main,subsdk0,sdk}/ # generated .cpp and table.cpp
private/bayonetta3/analysis/              # main.bin, subsdk0.bin, sdk.bin, etc.
private/bayonetta3/extract/romfs           # preparation's link to RomFS
private/bayonetta3-shaders-repro/spirv/    # .spv files and results.tsv
```

The runtime data root is a separate directory holding `analysis/`, `spirv/`, `extract/romfs/`, and a writable `user/` for saves. The desktop recipes link read-only inputs into it; Android copies their contents. Before reusing a runtime root, stop the game and back up its `user/` directory. Do not replace existing saves, delete unknown directories, or assume that changing a link updates another platform's data root.

## Linux / Steam Deck

### From a release (Steam Deck, or any x86-64 distribution)

The release archive `Bayonetta3Recomp-<version>-linux-x86_64.tar.gz` holds everything the setup needs besides your
dump: Clang and lld with a glibc sysroot, CMake and Ninja, SDL3, a Python with the recompiler's packages, the shader
translator (self-contained, no .NET), `spirv-val`, libopus, the Vulkan headers and a minimal ffmpeg for the
cutscenes. Nothing is installed on the system, so it works on SteamOS's read-only system in Desktop Mode, and nothing
is downloaded except the Kraken decoder's pinned source (about 100 KB, compiled on your machine). It needs a Vulkan
1.3 driver (Mesa RADV on the Deck) and glibc 2.35 or newer (an up-to-date SteamOS 3, Ubuntu 22.04, Fedora 36 and later).

1. Put your dump on the Deck (internal storage or the SD card): a folder with `exefs/` and `romfs/` of 1.2.0, and
   for the Old Picture Book `program1/` inside it, as described for Windows below.
2. Unpack the archive on a drive with about 25 GB free (the game data is hard-linked from the dump when both are on
   the same drive, at no extra space) and start `Bayonetta3Recomp.sh` (in Dolphin: right-click, Run in Konsole; or
   `./Bayonetta3Recomp.sh` in a terminal).
3. The launcher offers **Set up from my dump**: pick the dump folder. The setup checks the dump against the 1.2.0
   hashes, generates the code, translates and verifies the shaders, builds the game and links its data. It is
   resumable. `./Bayonetta3Recomp.sh --setup` does the same in the terminal.
4. **Start game**. For Game Mode, add `Bayonetta3Recomp.sh` to Steam as a non-Steam game and set its launch options
   to `--play`: Steam then starts the game directly, without the launcher.

Settings and saves work as on Windows (`private/b3.conf`, `private/win/run/b3/user/`); the game log is
`private/win/logs/game.log`. A newer release unpacked over the folder keeps all of it; then choose **Rebuild**.

### The developer route

Install Clang, CMake 3.25+, Ninja, the SDL3 development files, Vulkan headers and loader, and a working GPU driver, along with host libopus and ffmpeg. Vulkan headers must be compatible with the project's recorded 1.4.357 dependency. If SDL3 lives outside the normal search paths, set `CMAKE_PREFIX_PATH`.

To keep the whole game inside this folder (so it can be moved, and the dump is no longer needed), bring the dump in
first and point the generation at the result: `python3 tools/b3_data.py --dump <your dump folder>` hard-links
`exefs/` and `romfs/` into `private/data/b3/` (same drive: no extra space; `--mode copy` for another drive), then
use `--exefs private/data/b3/exefs --romfs private/data/b3/romfs` for `tools/prepare_bayonetta3.py`. Program 1 the
same way with `--to private/data/prog1`.

Build Release into the directory the player launcher uses; the development build helper defaults elsewhere:

```bash
cmake -S . -B private/build-deck -G Ninja \\
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \\
  -DCMAKE_CXX_COMPILER=clang++ -DNX_GAME=bayonetta3 \\
  -DNX_GEN_DIR="$PWD/private/bayonetta3/gen" -DNX_RECOMP_OPT=2 \\
  -DNX_VULKAN_INCLUDE="/path/to/Vulkan-Headers/include"
cmake --build private/build-deck --parallel 2 --target bayonetta3
mkdir -p private
[ -f private/b3.conf ] || cp config/b3.conf.example private/b3.conf
```

Edit `private/b3.conf`. On a Steam Deck, set `handheld=1` and `resolution=1280x720`, leave `fps_cap` empty, and keep `headless=0` and `mute=0`. The recorded Deck performance setup used 15 W and `gpu_clock=1600`; that clock setting is optional and specific to the Deck. If you are not reproducing the benchmark, start from your normal power settings.

```bash
unset NX_HEADLESS NX_MUTE
BUILD_DIR="$PWD/private/build-deck" \\
B3_SPIRV_DIR="$PWD/private/bayonetta3-shaders-repro/spirv" \\
  bash scripts/b3_play.sh
```

The launcher reads your config, creates a writable runtime root at `private/run-bayonetta3`, and starts the game. The log is `private/b3.log`. An optional settings UI is available as `bash scripts/b3_launcher.sh` (needs Python with Tk). For normal play use the player launcher; the timed experiment and benchmark scripts may mute, hide, automate or stop the game on purpose.

## Windows 10/11 x86-64 — from your dump to playing

On Windows one tool does the preparation, the build and the launch, natively, with no Git Bash and no Visual Studio:
`tools/b3_win.py`. You need:

- Your own dump of Bayonetta 3 with the 1.2.0 update applied: a folder with `exefs/` (`main`, `subsdk0`, `sdk`,
  `main.npdm`) and `romfs/`. For the Old Picture Book, also program `01004a4010fea001` from the 1.2.0 update in a
  `program1/` folder inside it (its own `exefs/` and `romfs/`); without it the book is skipped.
- Python 3.12+ from python.org (with Tcl/Tk for the launcher window).
- A Vulkan driver (every current GPU driver has one) and, for the cutscenes, the free "VP9 Video Extensions" from
  the Microsoft Store.

A release zip carries in `toolset/` the tools that would otherwise be built on your PC: the shader translator
(self-contained, no .NET), `spirv-val`, libopus, the Vulkan headers and the Python packages. The compiler toolchain —
llvm-mingw 20261020, CMake and Ninja — is downloaded by the setup from the official releases (hash-checked, about
200 MB, into `private/win/tools/`) unless it is already installed. From a plain checkout without `toolset/` the setup
also fetches MinGit and the .NET 10 SDK and builds those tools itself.

```bat
python tools\b3_win.py setup --dump D:\dumps\bayonetta3
python tools\b3_win.py play
```

`setup` checks the dump against the 1.2.0 hashes, fetches and builds the pinned helpers (the Kraken decoder, the
Ryujinx 1.3.2 shader translator, `spirv-val`, libopus, the Vulkan headers, the Python packages), generates the C++
for every module, translates and verifies the shaders, builds the game and links its data folders. It took 31
minutes on a 16-core laptop and is resumable: `--status` shows where it is, running it again continues, `--from <step>`
or `--only <step>` repeats steps (`deps verify data cpu shaders build link`). Your own installed tools are used when they
are on PATH or named with `--tool <name>=<path>` (`llvm_mingw`, `cmake`, `ninja`, `git`, `dotnet`). Everything it writes stays in
`private/win/`; your saves are in `private/win/run/b3/user/`.

The `data` step brings the dump's `exefs/` and `romfs/` into `private/win/data/`, so the folder holds the whole game
and the dump is no longer needed to play. By default it makes hard links (same drive: no extra space, the dump stays
whole); `--data copy` copies (another drive), `--data move` moves the files out of the dump, `--data keep` leaves
them where they are. After moving the whole folder, `play` notices that its data links point at the old place and
makes them again.

**Texture cache.** On a GPU without ASTC (Intel Arc, AMD, the Steam Deck) the game's ASTC textures are decoded on
the CPU the first time they load, as before, and a background thread at the lowest priority stores them as BC7 in
`private/win/run/b3/texcache/` (the log: `ASTC as BC7`). From the next load on they are read from there: a quarter
of the RGBA8 size, no decoding. Measured in the Prologue on an Arc B390: 1,256 textures and 2.7 s of CPU decoding
(worst 60-frame window 1.4 s) the first time, 70 textures and 56 ms (worst window 40 ms) the next; the Prologue's
cache is 1.1 GB. `astc_bc7=0` keeps RGBA8, `texcache_mb=` caps the cache (default 8192). Phones with Adreno read
ASTC themselves and do not use it. BC7 here is one mode (one subset): 45 dB against the ASTC decode on average,
about 25 dB on the rare block with several distinct colours.

**Mods** go in `private/win/run/b3/mods/<name>/romfs/` (or straight in `mods/<name>/`), with the same paths as the
game's RomFS: a file there replaces the game's file of that name. A mod packed for a console or another emulator works
as it is: the first `romfs/` within four levels is used (`atmosphere/contents/<title id>/romfs`, `<title id>/romfs`).
Folders apply in the order `mods/order.txt` lists them (one folder name a line), then the others in name order, and a
later one wins; rename a folder to start with `-` to switch it off. The launcher's Mods tab does all of this: add a mod
from its .zip or folder, switch it on or off, and move it up or down. Code patches (`exefs/`, `.ips`, `.pchtxt`) patch
the Switch program and do nothing here. The log lists the mods it found (`fs: mod ...`).

**A release zip** of this checkout: `python tools\b3_win.py package [--python <a Python install folder>]
[--toolset <folder>]` writes `dist\Bayonetta3Recomp-<version>-windows-x64.zip` (on Linux:
`-linux-x86_64.tar.gz` with `Bayonetta3Recomp.sh`) and its `.sha256`; `tools/build_toolset.py` makes the toolset. It holds the tracked files only (it refuses
anything under `private/` and game file formats) and a `Bayonetta3Recomp.cmd` that, on first double-click, asks for
the dump folder, runs the setup and then opens the launcher (`b3_win.py first-run`). Make releases from the public
tree: its CI (`.github/workflows/ci.yml`) runs the unit tests, builds this zip with a bundled Python on every push,
and a `v*` tag publishes it as a GitHub Release.

**After updating** (a pull, or a newer release unpacked over the folder): `python tools\b3_win.py update`, or
**Rebuild** in the launcher. It applies new runtime hooks to the generated code in place and rebuilds only what
changed; the generated C++ and the shaders are made again only when the tools that make them changed
(`update --check` says what it will do). The launcher's home screen offers **Set up from my dump** while nothing is
built, with the setup's progress in the window.

`play` reads `private/b3.conf` (created from `config/b3.conf.example` on first launch) and starts the game. When the
picture book is opened it starts the Bayonetta Origins prologue and returns to Bayonetta 3 afterwards; both share
the save. `scripts\b3_win_launcher.cmd` opens the launcher window (settings, controller check, Play) instead.

Keyboard and mouse work alongside a controller. Click into the window to turn the camera with the mouse (F10 or
switching windows releases it). Every input can be rebound in `private/b3.conf` with `key_<input>=<key>,...`; the
file lists the inputs, the key names and the defaults. F11 or Alt+Enter toggles fullscreen.

**F1** (or + and − together on a controller) opens the settings panel over the game: window mode, resolution, frame
rate, v-sync, anisotropic filtering, FSR, model detail, film grain, controller layout, the mouse camera and sound. The arrows or
the d-pad choose and change, Esc or B closes; the game is paused while it is open. Changes are saved to
`private/b3.conf`; window mode, model detail, film grain and the mouse apply at once, the others say "(next start)". Model detail
`Highest` (`model_lod=highest`) keeps every model with more than one level of detail at its finest at any distance. Film grain `Off` (`film_grain=off`) drops the grain the
game lays over the picture, from the next scene on (the same field the community "No Film Grain" mod removes).

In the picture book's prologue, + offers to close the book; that returns to Bayonetta 3, where Continue goes back to
the hub. For Discord Rich Presence, create an application at discord.com/developers (its name is what your friends
see) and put its numeric id in `private/b3.conf` as `discord_app_id=`.

### The developer route (Git Bash)

The rest of this section is the older manual route, for working on the runtime itself. Use Git Bash, CMake, Ninja
and LLVM-MinGW. Add its `bin` directory to PATH so the prefixed compilers and their runtime DLLs are both available. In the commands below, `MINGW` is the LLVM-MinGW install root and `VKH` the Vulkan SDK or header root containing `Include/vulkan` (or `include/vulkan` on a case-insensitive filesystem). The graphics driver supplies the Vulkan loader.

```bash
MINGW="/path/to/llvm-mingw"
VKH="/path/to/Vulkan-Headers"
export PATH="$MINGW/bin:$PATH"
cmake -S . -B private/build-win -G Ninja \\
  -DCMAKE_BUILD_TYPE=Release -DNX_GAME=bayonetta3 \\
  -DCMAKE_C_COMPILER="$MINGW/bin/x86_64-w64-mingw32-clang.exe" \\
  -DCMAKE_CXX_COMPILER="$MINGW/bin/x86_64-w64-mingw32-clang++.exe" \\
  -DNX_GEN_DIR="$(cygpath -m "$PWD/private/bayonetta3/gen")" \\
  -DNX_VULKAN_SDK="$(cygpath -m "$VKH")" -DNX_RECOMP_OPT=2
cmake --build private/build-win --parallel 2 --target bayonetta3
bash tools/build_opus_win.sh --jobs 2
cp private/build-opus-win/libopus.dll private/build-win/libopus.dll
mkdir -p private
[ -f private/b3.conf ] || cp config/b3.conf.example private/b3.conf
```

Use the prefixed MinGW compiler drivers rather than an unrelated `clang` from PATH. Windows needs `libopus.dll` next to the executable; video uses Media Foundation and needs VP9 decoding support. In `private/b3.conf`, a useful starting point is `resolution=1600x900`, `headless=0`, `mute=0`. `resolution` is the host window; the game renders at its console size, 1600x900 docked or 1280x720 with `handheld=1`, and the picture is scaled to the window, so a larger window is not a higher internal resolution (`render_res` and `render_scale` do not change it for this game).

```bash
export B3_WIN_GAME_ROOT="$PWD/private/game"
export B3_SPIRV_DIR="$PWD/private/bayonetta3-shaders-repro/spirv"
export B3_WIN_DATA_ROOT="$PWD/private/run-win"
export NX_DATA_ROOT="$(cygpath -w "$B3_WIN_DATA_ROOT")"
export NX_LOG="$(cygpath -w "$PWD/private/b3-win.log")"
unset NX_HEADLESS NX_MUTE
bash scripts/b3_win_play.sh
```

The `B3_*` paths above are consumed by Bash, and the helper converts them when it creates NTFS junctions. Paths exported directly to the native runtime as `NX_*` must be Windows-native paths, since Git Bash does not reliably convert an arbitrary environment-variable path. The shader bundle is selected with `B3_SPIRV_DIR`; there is no `NX_SPIRV_DIR`. Existing runtime links are preserved, so inspect their targets when using an old data root.

## macOS (Apple Silicon)

Install CMake, Ninja, SDL3, Vulkan headers/loader, MoltenVK, libopus and ffmpeg, for example with Homebrew. The Vulkan loader must be able to find the MoltenVK ICD; if it cannot, follow your installed SDK or Homebrew instructions. Use the generated CPU tree and shader bundle prepared above.

```bash
brew install cmake ninja sdl3 vulkan-headers vulkan-loader molten-vk opus ffmpeg
cmake -S . -B private/build-mac -G Ninja \\
  -DCMAKE_BUILD_TYPE=Release -DNX_GAME=bayonetta3 \\
  -DNX_GEN_DIR="$PWD/private/bayonetta3/gen" -DNX_RECOMP_OPT=2 \\
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build private/build-mac --parallel 2 --target bayonetta3
```

Use a fresh runtime directory for the setup below. If the path already exists, inspect it or choose another name rather than replacing its saves or links. macOS `ln` has no `-T` option.

```bash
MAC_RUN="$PWD/private/run-mac-player"
[ ! -e "$MAC_RUN" ] || { echo 'Choose a fresh MAC_RUN directory'; exit 1; }
mkdir -p "$MAC_RUN/extract"
ln -s "$PWD/private/bayonetta3/analysis" "$MAC_RUN/analysis"
ln -s "$PWD/private/game/RomFS" "$MAC_RUN/extract/romfs"
ln -s "$PWD/private/bayonetta3-shaders-repro/spirv" "$MAC_RUN/spirv"
```

Later launches reuse the same `MAC_RUN` path and need only the launch command:

```bash
unset NX_HEADLESS NX_MUTE
NX_DATA_ROOT="$MAC_RUN" NX_LOG="$PWD/private/b3-mac.log" \\
NX_HANDHELD=1 NX_RESOLUTION=1280x720 \\
  ./private/build-mac/bayonetta3
```

This runs the normal visible, audible executable directly and does not load `private/b3.conf`, so export any `NX_*` settings you want. Do not use `NX_HEADLESS=0` or `NX_MUTE=0`: these switches are enabled by their presence. The macOS gameplay measurements include older diagnostic runs, and this visible-window recipe was reviewed against the source rather than rerun as a fresh visible session for the documentation. Windows `render_res` measurements should not be assumed to apply unchanged to macOS.

## Android arm64 — build, install and play

### From a release (no PC needed)

The release's `android-arm64.apk` carries the launcher and the setup tools (clang and lld built to run on Android, Python, the shader translator, spirv-val), not the game. The phone turns your own dump into the game itself:

1. Install the APK and open it. Allow "All files access" when it asks (the game folder lives in Internal storage).
2. Copy the `exefs/` and `romfs/` folders of your Bayonetta 3 1.2.0 dump into `Internal storage/Bayonetta 3/dump` (from a PC over USB, or with a file manager).
3. Tap **SET UP**, then **Start setup**. Keep the phone charging and the screen open: the setup moves the dump into the game folder (a rename, so no second copy), translates the game's code and shaders, and compiles them on the phone. That takes about 30 to 60 minutes on a recent phone and needs about 6 GB free. If it stops, **Start setup** carries on from the last finished step.
4. When it says the game is ready, go back and press **START**.

The setup downloads one file: the source of the Kraken decoder (powzix/ooz at a pinned commit), whose license keeps it out of the APK; the phone compiles it like the rest. The game itself uses no network. The built game stays inside the app (`files/game/libbayonetta3.so`), and saves go to `Internal storage/Bayonetta 3/user`. To install a newer release over this one, both must be signed with the same key: the releases are, but an APK you built yourself is not, so back up `user/` before switching between them.

### The developer route

On Linux, these commands build and package the APK; the Opus helper needs a Linux host because it uses the NDK's `linux-x86_64` strip tool. Install CMake, Ninja, Java/keytool, zip, adb, Android SDK build-tools and an Android platform API of 35 or newer, plus an NDK (the project used r28b) and recent Vulkan headers. The generated application targets Android API 29+, arm64-v8a. If more than one adb device is connected, select the intended phone with `ANDROID_SERIAL`.

```bash
export ANDROID_NDK_ROOT="/path/to/android-ndk"
export ANDROID_SDK_ROOT="/path/to/android-sdk"
export VULKAN_HEADERS="/path/to/Vulkan-Headers/include"
export PATH="$ANDROID_SDK_ROOT/platform-tools:$PATH"
bash tools/build_opus_android.sh --jobs 2
bash android/build_bayonetta3.sh \\
  --gen-dir "$PWD/private/bayonetta3/gen" --jobs 2 --apk
adb install -r private/build-android/apk/bayonetta3.apk
```

On Windows, `android\build_bayonetta3.ps1` does the same from PowerShell: it uses the generated code from the Windows setup (`private\win\cpu\b3\gen`), takes the SDK from `ANDROID_SDK_ROOT` (newest NDK, build-tools and platform 35+), needs CMake, Ninja and a JDK, and `-Install [-Serial <device>]` also installs the APK and grants the storage permission. libopus still comes from the Linux helper above (`private/toolchains/opus-android/arm64-v8a/libopus.so`); without it the script warns and the game stops at startup.

```powershell
android\build_bayonetta3.ps1 -Install
```

This builds the native library and packages libopus into a locally signed development APK. Installing it does not supply the game files. `adb install -r` requires a compatible signature, so back up the saves before uninstalling an existing installation to resolve a signing mismatch.

The commands below use the shared storage root `/sdcard/Bayonetta 3`. Stop the app and back up any existing `user/` folder before updating data; none of these pushes replaces `user/`. Push contents with the trailing `/.` so you do not end up with `analysis/analysis` or `romfs/RomFS` nesting:

```bash
adb shell am force-stop io.github.bayonetta3recomp
adb shell 'mkdir -p "/sdcard/Bayonetta 3/analysis" "/sdcard/Bayonetta 3/spirv" "/sdcard/Bayonetta 3/extract/romfs"'
adb push private/bayonetta3/analysis/. '/sdcard/Bayonetta 3/analysis/'
adb push private/bayonetta3-shaders-repro/spirv/. '/sdcard/Bayonetta 3/spirv/'
adb push private/game/RomFS/. '/sdcard/Bayonetta 3/extract/romfs/'
```

Create a local `private/android-config.txt` with exactly these lines, without spaces around `=` or inline comments. If the device already has a config, pull it and edit it rather than replacing it outright:

```ini
resolution=1280x720
NX_HANDHELD=1
NX_FPS_CAP=30
NX_GPU_TURBO=1
```

Android's platform window-size parser reads the plain `resolution` key, while runtime switches use exact `NX_*` names; the lowercase desktop player-config keys are not interchangeable with them. Android defaults to 30 FPS for sustained play, and `NX_FPS_CAP=60` removes that floor without guaranteeing 60 FPS. `NX_FPS_CAP=60` together with `NX_DISPLAY_HZ=40` paces an even 40 FPS on a 120 Hz panel instead; on the phone measured so far it holds only until the device heats up (see [PERFORMANCE.md](PERFORMANCE.md)). The latest recorded phone setup keeps `NX_GPU_TURBO=1`; heating and device drivers still affect sustained performance.

```bash
adb push private/android-config.txt '/sdcard/Bayonetta 3/config.txt'
adb shell am start -a android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION \\
  -d package:io.github.bayonetta3recomp
# On the phone, enable all-files access, then return to the game.
adb shell am start -n io.github.bayonetta3recomp/android.app.NativeActivity
```

The native shell has no game importer. Its data root lookup prefers the selected root, then a populated app-specific external folder, then the shared `Bayonetta 3` folder. An older app-specific folder that already contains `analysis/main.bin` may take priority over the new shared files, so check the startup log before moving anything. For diagnostics, run `adb logcat -s bayonetta3`.

## Controls, saves and chapter selection

Use a controller for normal gameplay. Buttons follow Nintendo positions by default, and `pad_swap=1` on desktop swaps A/B and X/Y for Xbox labels. On keyboard, WASD or the arrow keys move; Space is A, Shift is B, J is X, K is Y, Q/E are L/R, Z/C are ZL/ZR, Enter is + and Escape is −. On Windows the mouse also turns the camera once you click into the window, the left and right buttons are Y and X, V and the middle button are the stick clicks, Numpad 8/4/5/6 turn the camera, and `key_<input>=` in `private/b3.conf` rebinds anything. Android supports touch controls as well as controllers. The on-screen layout follows the two Joy-Cons: the left stick with the D-pad below it, the face buttons with the right stick below them, L/R and ZL/ZR in the top corners and −/+ in the middle. Both sticks float (they start where the thumb lands in their half of the screen and spring back when released); the D-pad takes diagonals and lets the thumb slide between directions. The controls hide while a controller is in use. `NX_TOUCH_SHOW=0`, `NX_TOUCH_SIZE` (50–160), `NX_TOUCH_OPACITY` (10–100), `NX_TOUCH_MIRROR=1` and `NX_TOUCH_LAYOUT` in `config.txt` hide, scale, dim, mirror or move them.

A first boot with an empty `user/` folder can start a new game. Quit cleanly and back up `user/` before moving between builds or platforms. A progressed personal save may expose more chapters; it is not a build dependency and is not distributed with the project.

To reach an unlocked Chapter 8, continue into the hub and use the wall map/dartboard: hold L, aim the stick at the purple chapter node, press A, then choose Chapter Select/checkpoint and confirm with A.

The **red diamond nodes** on the same board are the Phenomenal Remnants, each chapter's optional challenge stage. They are ordinary main-game stages and are entered the same way: hold L, aim at the node, A, Start, pick a character, A. They are also the stage behind issue #2. The switch that picks which stage to load has 270 entries, and the recompiler recovered only the first 256 of them, so a stage whose index was above 255 and pointed somewhere new had no case, called into the middle of a guest function, and the process exited instead of loading the stage. That is fixed, and the stage loads and plays. The red diamond is not the same thing as the records/book interaction, which starts the second guest program (the Bayonetta Origins prologue). On Windows that program builds and renders, and `scripts/b3_win_play.sh` performs the program switch, but the round trip from the book has not been played through. A known Prologue/end-of-cutscene loading or progression issue remains; reaching Chapter 8 through chapter selection is a useful route around that section, not proof of an uninterrupted full-game playthrough. Do not overwrite a new save to follow that route.

## If the screen stays black or launch exits

1. Read the startup log and verify the actual runtime data root. It must contain `analysis/main.bin`, the other matching module images, the full RomFS under `extract/romfs/` (including `root_000.pkz`) and a populated `spirv/` with `results.tsv`. Check link targets and Android folder nesting, not merely that directory names exist.
2. Confirm the executable and generated modules are from the same matching 1.2.0 inputs. A standalone test build without generated code is not the game executable.
3. Unset inherited `NX_HEADLESS`/`NX_MUTE` and diagnostic/automation settings, and inspect the player config. A value of `0` does not disable every runtime environment switch.
4. Check Vulkan device/loader errors and shader errors. Linux/macOS cutscenes require ffmpeg on PATH; Windows requires working Media Foundation VP9 support. `hwopus: host libopus unavailable` or the Windows DLL error means the host Opus dependency is missing.
5. On Android, check all-files access and the `android: chosen ... own ... shared ...` startup message. On Windows, verify native `NX_*` paths and keep LLVM-MinGW runtime DLLs available. A memory-limit exit is not fixed by forcing a low 8 GiB ceiling; leave the default unless you have measured a suitable limit.
6. Distinguish a first-boot/data-path problem from the known later Prologue progression issue. Keep your save backup and report the scene, platform, settings and relevant sanitized log excerpt.
