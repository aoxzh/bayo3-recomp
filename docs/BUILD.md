# Building and testing

Bayonetta 3 Recomp targets version 1.2.0 with a static ARM-to-C++ recompiler and a Horizon/NVN runtime using Vulkan. The development working tree has reached actual combat on Windows, Steam Deck, Android and macOS, and on Windows has reached Chapter 8 through the hub's chapter selector. [Measured gameplay and performance](PERFORMANCE.md) gives dates, hardware and limitations. The normal Prologue story transition still has a loading issue, so a complete playthrough is not claimed.

For a normal game setup, follow [Playing and platform setup](PLAYING.md): prepare the three CPU modules, build, translate and publish the shaders locally, build the game for your host, arrange the runtime data root, then launch with input and sound enabled. This document covers the preparation and toolchain steps in detail, plus separate developer checks. Independent test success and a fresh all-platform end-to-end rebuild are distinct from the existing gameplay evidence.

No game, generated game binaries or game assets are provided, apart from the screenshots in the documentation. You must supply a legally obtained copy of the supported game and any tools and libraries it separately requires. Do not redistribute game-derived files. The standalone tests below do not need the game.

## Standalone tests

You need CMake 3.25 or newer and a C/C++ toolchain with C++20 support. When `NX_GEN_DIR` contains no generated game code, CMake configures only the standalone test targets. On a POSIX shell, for example:

```sh
cmake -S . -B private/build-standalone -DNX_TESTS=ON \\
  -DNX_GEN_DIR="$PWD/private/empty-generated-for-tests"
cmake --build private/build-standalone
ctest --test-dir private/build-standalone --output-on-failure
```

Choose an `NX_GEN_DIR` path that contains no generated modules. `NX_TESTS` is off by default. Which tests run varies by operating system and available tools: some Linux tests need Python and Vulkan headers, and some need Pillow, Bash, SDL3 or particular generated files. CTest may report skips, so inspect the list and results on your machine rather than assuming every test was registered. Passing tests checks their isolated contracts; it does not establish that the game builds, renders correctly or is playable.

### Windows standalone test baseline

On 2026-11-04, a game-free Release build on Windows used CMake 4.4.4, Ninja 1.13.2, LLVM-mingw Clang 23.1.4, Python 3.14 and Pillow 13.0.0. CTest reported 12 passed and one skipped: `ubsan_smoke` skipped because the build did not enable UBSan. The included autodrive registration exercises synthetic cases only, and the omitted game-derived image fixtures were not tested. These results are a baseline for this snapshot, not a game-build or playability result.

With CMake, Ninja, Git, Git Bash, Python and Pillow on `PATH`, also add the LLVM-mingw `bin` directory so the test executables can find its runtime DLLs. From Git Bash in the repository root, replacing the LLVM-mingw placeholder with your installation path:

```sh
export PATH="/path/to/llvm-mingw/bin:$PATH"
cmake -S . -B "$TEMP/b3-standalone" -G Ninja \\
  -DNX_TESTS=ON "-DNX_GEN_DIR=$TEMP/b3-empty-generated" \\
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$TEMP/b3-standalone" --parallel 4
ctest --test-dir "$TEMP/b3-standalone" --output-on-failure
```

Use a fresh build directory and an `NX_GEN_DIR` with no generated modules. The preparation tests, including the mocked end-to-end preparation, also run under native Windows Python.

## What a game build needs

- A legally obtained, locally extracted Bayonetta 3 1.2.0 main-program ExeFS and RomFS matching `config/bayonetta3-1.2.0.json`. Preparation validates the configured identity, build IDs and module layout. The main program needs generated `main`, `subsdk0` and `sdk` modules.
- Python and the dependencies pinned in `requirements-bayonetta3.txt` (`lz4` and `capstone`). The preparation script does not download them; installing them is a separate step.
- Python on Linux, macOS or native Windows for `tools/prepare_bayonetta3.py`. On Windows it locks its output with `msvcrt` and links the RomFS and ExeFS with directory junctions when symlinks are not permitted, so it needs neither Developer Mode nor elevation. Its tests pass on native Windows. A complete generation has been run on native Windows for the main program and for program 1 (the picture book's program): the generated code matches a Linux run of the same recompiler except for disassembly comments, and a main-program build from it reaches the title screen. `requirements-bayonetta3.txt` pins `lz4==4.4.4`, which has no Python 3.14 wheel for Windows and does not build without MSVC; lz4 4.4.5 installs as a wheel there, and the recompiler only calls its block decompression.
- CMake 3.25+, a C++20 compiler, and the platform's runtime dependencies. The Linux helper uses Ninja and defaults to Clang. Linux and macOS game targets use SDL3 and Vulkan headers and loader; macOS also needs a Vulkan implementation such as MoltenVK. The Linux target requests `x86-64-v3`. Windows code uses XInput, XAudio and Media Foundation. Android has a separate NDK build path. These are source-level requirements, not platform support guarantees.
- A compatible shader pipeline and its external inputs, described below. A successful C++ generation step alone does not provide the shaders.

## Prepare the local game

The following is an illustrative POSIX example. Replace the placeholder with your own extracted files and keep all outputs private. Run it from the repository root. It creates a Python environment and installs the pinned dependencies, which may need access to your configured package source:

```sh
python3 -m venv private/venv
private/venv/bin/python -m pip install -r requirements-bayonetta3.txt
GAME_DIR="/path/to/your/Bayonetta3-1.2.0"
PYTHON="$PWD/private/venv/bin/python"
"$PYTHON" tools/prepare_bayonetta3.py --help
"$PYTHON" tools/prepare_bayonetta3.py --inspect-only \\
  --exefs "$GAME_DIR/ExeFS" --romfs "$GAME_DIR/RomFS" \\
  --config "$PWD/config/bayonetta3-1.2.0.json" \\
  --output "$PWD/private/bayonetta3" --python "$PYTHON"
```

Inspection writes files. `--inspect-only` validates the configured inputs and publishes unrelocated analysis images, a manifest and ExeFS/RomFS links under the output directory. The manifest records local paths and hashes. Treat the output and the referenced game data as private and do not attach them to an issue. The tool takes a lock and refuses an unowned or modified output tree, or changed inputs. If it refuses a directory, read the error and choose a fresh private output; do not force it over unrelated data.

To generate the full main-program C++ modules after inspection, use the same arguments and output, replacing `--inspect-only` with `--generate`:

```sh
"$PYTHON" tools/prepare_bayonetta3.py --generate \\
  --exefs "$GAME_DIR/ExeFS" --romfs "$GAME_DIR/RomFS" \\
  --config "$PWD/config/bayonetta3-1.2.0.json" \\
  --output "$PWD/private/bayonetta3" --python "$PYTHON"
```

`--module` can be repeated to generate individual modules, but a full main-program build requires all three. Run `--help` for the current flags; script defaults and external paths may differ across checkouts. Preparing the optional second program is a separate task that the commands above do not establish.

## Presentation shader regeneration

The renderer's presentation pass has two small, non-game GLSL sources in [`runtime/shaders`](../runtime/shaders), compiled into [`runtime/src/shaders_spv.h`](../runtime/src/shaders_spv.h). The original GLSL was unavailable, so these sources were reconstructed from the formerly embedded SPIR-V and the header was regenerated. This is separate from the game-derived shader pipeline below.

With `glslc` from the Vulkan SDK available, you can check whether the embedded header matches a fresh compilation without modifying it:

```sh
python3 tools/gen_shaders.py --check /path/to/glslc
```

After intentionally editing these sources, omit `--check` to regenerate the header. Missing required source files make the generator stop without overwriting the header. The comparison is byte-for-byte, and compiler versions may produce different valid SPIR-V, so use the same compiler before treating a mismatch as a source change. This snapshot was generated on Windows with Vulkan SDK 1.4.341.0, shaderc v2026.1, targeting Vulkan 1.0 with `-O`.

Both the former and the regenerated modules passed SPIRV-Tools v2026.1 validation for Vulkan 1.0. An instruction-level comparison mapped every ID bijectively: all function instructions and operands matched in order, and pre-function declarations matched as a multiset. IDs, declaration order and generator metadata differ. The regenerated vertex and fragment modules have SHA-256 `1c43d003e2640eb512f98cd76df4f97c0ae616621dffc7333f032d13c30e10d1` and `1bdbb621042e22ea51c58b9357d99043b1519510f9e3b15fd7a394fca4e073cf`, respectively. The original GLSL was not recovered and no GPU rendering comparison was performed.

## Game shader generation and external dependencies

`tools/bayonetta3_shaders.py` handles the Bayonetta 3 shader path. The pipeline requires inputs that are not bundled:

- A locally built Kraken decoder worker compatible with the tool's protocol. For each request the worker receives two little-endian unsigned 64-bit values (compressed length and expected decompressed length), followed by the compressed bytes, and must return a little-endian signed 64-bit result equal to the expected length, followed by exactly that many decompressed bytes. It must support multiple requests on one process's standard input/output. An adapter source is included below, but its external decoder and a game-compatible worker are not supplied or fully validated.
- Compatible Ryujinx shader source and the `Ryujinx.Common.dll` and `Spv.Generator.dll` libraries. The harness targets .NET 10.0 and needs a compatible .NET SDK.
- `spirv-val` and a C++20 compiler for verification, plus the matching local game files. Extraction checks module identities and the `RomFS/root_000.pkz` hash; its scope is that archive, not all possible shaders.

### Historical dependency clues

A historical build record identifies .NET SDK 10.0.402 and a [vendored Ryujinx source snapshot](https://github.com/enginefuture/bayonetta3-mobile/tree/e5ce11694d380d70b53c4ec7c4f789b458058d3e/third_party/blade2-mobile/dm/third_party/ryujinx) at commit `e5ce11694d380d70b53c4ec7c4f789b458058d3e`. Its README describes the subset as unmodified Ryujinx 1.3.2 under MIT. A local checkout of that commit matched the recorded hashes for all 190 tracked shader C# files; the original upstream Ryujinx commit has not been established. In that snapshot, the source path corresponding to `--ryu-shader` is `src/Ryujinx.Graphics.Shader` below the linked directory. The recorded support DLL SHA-256 values were:

- `Ryujinx.Common.dll`: `af569e0f6e29910663cb199fb4e3e1a0b0873d75167ab911d0999e4706b48890`
- `Spv.Generator.dll`: `1b1f17468ed643aef2fdf470ffbe7776f61e4ba04fdd39948edd0d976cfeb051`

These fingerprints identify historical local outputs, not artifacts provided by this repository. The current harness also differs from the historically recorded one, and the rebuilt DLLs in the experiment below differ from those historical fingerprints.

### Build the translator without game files

Obtain the pinned public source in a new private toolchain directory (this needs Git and network access). A sparse checkout avoids downloading unrelated working-tree content, and the commit is fixed rather than following a moving branch:

```sh
git clone --filter=blob:none --no-checkout \\
  https://github.com/enginefuture/bayonetta3-mobile.git private/toolchains/bayonetta3-mobile
git -C private/toolchains/bayonetta3-mobile sparse-checkout init --cone
git -C private/toolchains/bayonetta3-mobile sparse-checkout set \\
  third_party/blade2-mobile/dm/third_party/ryujinx
git -C private/toolchains/bayonetta3-mobile checkout --detach \\
  e5ce11694d380d70b53c4ec7c4f789b458058d3e
```

Use `private/toolchains/bayonetta3-mobile/third_party/blade2-mobile/dm/third_party/ryujinx` as `RYU_ROOT`. If the destination already exists, inspect its revision instead of overwriting it. Install .NET SDK 10.0.402 from Microsoft's official distribution and Vulkan SDK/SPIRV-Tools for `spirv-val`; the decoder adapter build below also needs LLVM-MinGW on Windows or a compatible C++ compiler on its target host.

The commands below were tested in Windows Git Bash with .NET SDK 10.0.402, starting from a clean copy of the pinned Ryujinx subtree above. They build the support libraries and this project's current translator harness, and do not extract or translate game shaders. Obtain that source separately, set `RYU_ROOT` to its `ryujinx` directory, and run the commands from this repository's root. The source's dependencies may need to be restored from your configured NuGet source. `cygpath` converts Git Bash paths for the Windows .NET SDK.

```sh
RYU_ROOT="/path/to/the/pinned/ryujinx"
dotnet --version
dotnet build "$RYU_ROOT/src/Ryujinx.Graphics.Shader/Ryujinx.Graphics.Shader.csproj" \\
  -c Release -p:RunAnalyzers=false

mkdir -p private
HARNESS_DIR="$(mktemp -d "$PWD/private/shader-harness.XXXXXX")"
cp tools/bayonetta3_shaders/ShaderHarness.csproj \\
  tools/bayonetta3_shaders/Program.cs \\
  tools/bayonetta3_shaders/StaticGpuAccessor.cs \\
  tools/bayonetta3_shaders/Disassemble.cs "$HARNESS_DIR/"
RYU_SHADER="$(cygpath -w "$RYU_ROOT/src/Ryujinx.Graphics.Shader")"
RYU_LIB="$(cygpath -w "$RYU_ROOT/src/Ryujinx.Graphics.Shader/bin/Release/net10.0")"
dotnet build "$HARNESS_DIR/ShaderHarness.csproj" -c Release \\
  "-p:RyuShader=$RYU_SHADER" "-p:RyuLib=$RYU_LIB" -p:RunAnalyzers=false
```

Both builds passed in the test, and starting the resulting harness without arguments printed its usage. With `spirv-val` from the Vulkan SDK, you can also translate an artificial no-op compute shader and validate its SPIR-V for Vulkan 1.2. This test creates its inputs in a temporary directory and needs no game files:

```sh
python tools/test_shader_harness.py \\
  --harness "$HARNESS_DIR/bin/Release/net10.0/Ryujinx.Graphics.Shader.dll" \\
  --spirv-val "/path/to/spirv-val"
```

That synthetic test passed with .NET SDK 10.0.402 and SPIRV-Tools v2026.1. The harness references the two support DLLs directly, and its generated dependency manifest did not list the NuGet dependencies of `Ryujinx.Common`; whether every dependency needed by more complex translations is available remains unverified. The synthetic test does not use game data or verify playback; a separate limited local-input experiment is described below. The fresh harness directory is private and can contain build paths, so inspect it before sharing any artifacts.

### Experimental decoder worker

A [public worker example](https://github.com/enginefuture/bayonetta3-mobile/blob/e5ce11694d380d70b53c4ec7c4f789b458058d3e/tools/rom_decoder_worker.cpp) describes the worker protocol but expects a different decoder C ABI. The included [`kraken_worker_ooz.cpp`](../tools/bayonetta3_shaders/kraken_worker_ooz.cpp) adapts that MIT-licensed worker to the C++ API of [powzix/ooz](https://github.com/powzix/ooz/tree/05038060aa68f9187ae9923b2388ca8db40e58d1) and uses binary-mode pipes on Windows. The external ooz source and compiled worker are not included.

The ooz [`kraken.cpp`](https://github.com/powzix/ooz/blob/05038060aa68f9187ae9923b2388ca8db40e58d1/kraken.cpp) file states GPL-3.0-or-later. Other files needed to build that revision do not all carry their own license notices, so check their terms and those of the combined artifact before distributing it. The ooz decoder warns that it is not fuzz safe; use it only with inputs you trust.

The following was tested on Windows Git Bash with LLVM-MinGW Clang 23.1.4, using the linked ooz revision. It builds into a fresh private directory and links the external decoder statically; the `kraken.cpp` compile emitted warnings. Obtain that source separately, replace `OOZ_ROOT` and ensure `clang++` and `python` are on PATH:

```sh
OOZ_ROOT="/path/to/the/pinned/ooz"
mkdir -p private
WORKER_BUILD="$(mktemp -d "$PWD/private/kraken-worker.XXXXXX")"
clang++ -O2 -include sys/stat.h -Dmain=ooz_cli_main \\
  -c "$OOZ_ROOT/kraken.cpp" -o "$WORKER_BUILD/kraken.o"
clang++ -O2 -static tools/bayonetta3_shaders/kraken_worker_ooz.cpp \\
  "$WORKER_BUILD/kraken.o" "$OOZ_ROOT/bitknit.cpp" "$OOZ_ROOT/lzna.cpp" \\
  -o "$WORKER_BUILD/kraken-worker.exe"
python tools/test_kraken_worker.py --worker "$WORKER_BUILD/kraken-worker.exe"
```

The included test passed synthetic raw and special-fill blocks, repeated requests, malformed frames and calls through this project's decoder client. In a separate local experiment, the adapter decoded one compressed archive member from the hash-checked 1.2.0 input, and six extracted shader identifiers matched 16-hex-digit SHA-1 prefixes in a prior local cache. One of those shaders was translated by the rebuilt harness and passed `spirv-val` for Vulkan 1.2, but its output was not byte-identical to the previously cached module. Temporary game-derived files were removed after the experiment. This sample does not verify every member, semantic equivalence, the complete pipeline, rendering or licensing. Linux and macOS builds of this adapter have not been verified.

Before attempting it, check the actual flags and actions:

```sh
python3 tools/bayonetta3_shaders.py --help
```

The following illustrates the explicit inputs, using `GAME_DIR` and `PYTHON` from the preparation example. Replace every placeholder with a compatible local tool or directory; this command is not a verified recipe:

```sh
"$PYTHON" tools/bayonetta3_shaders.py all \\
  --game "$GAME_DIR" --work "$PWD/private/shader-work-local" \\
  --decoder "/path/to/compatible/kraken-worker" \\
  --dotnet "/path/to/dotnet" \\
  --ryu-shader "/path/to/Ryujinx.Graphics.Shader/source" \\
  --ryu-lib "/path/to/directory/with/required/dlls" \\
  --spirv-val "/path/to/spirv-val"
```

The work directory must be under `private/` and outside the managed `private/bayonetta3` tree. The `all` action performs `extract`, `build`, `translate` and `verify`; `publish` is a separate action that copies verified results to a private local directory given by `--publish-dir`. These actions write game-derived data, logs and provenance that includes local paths. Verification checks static Vulkan and runtime shader contracts, not rendered gameplay. The external worker and Ryujinx inputs must be obtained or built with the pinned recipes above. Their builds, synthetic checks and a limited real-game sample were exercised, but the entire archive-to-game pipeline was not rerun from scratch during export. Check the accepted and rejected translation counts and the verification output on your inputs, then publish the verified local bundle for the player scripts as shown below.

### Publish the verified shaders locally

Use the same work directory as the `all` command. `publish` here copies verified outputs into another private local directory; it does not upload anything:

```sh
"$PYTHON" tools/bayonetta3_shaders.py publish \\
  --work "$PWD/private/shader-work-local" \\
  --publish-dir "$PWD/private/bayonetta3-shaders-repro"
export B3_SPIRV_DIR="$PWD/private/bayonetta3-shaders-repro/spirv"
test -f "$B3_SPIRV_DIR/results.tsv"
```

The destination must be new. Do not delete existing outputs or saves to bypass a refusal. Every platform needs this `spirv` directory and its table, not just a folder of `.spv` files. The player-script default uses a different historical bundle name, so set `B3_SPIRV_DIR` explicitly as above.

## Linux build and launch

Once all three modules and suitable shaders exist, the Linux helper shows the intended build flow. Set the paths explicitly rather than relying on machine-specific defaults:

```sh
BUILD_DIR="$PWD/private/build-deck" \\
GEN_DIR="$PWD/private/bayonetta3/gen" \\
SDL_PREFIX="/path/to/your/SDL3/install/prefix" \\
VULKAN_HEADERS="/path/to/your/Vulkan-Headers/include" \\
CC=clang CXX=clang++ JOBS=6 RECOMP_OPT=2 \\
  bash scripts/build_linux.sh -DCMAKE_BUILD_TYPE=Release

BUILD_DIR="$PWD/private/build-deck" \\
B3_SPIRV_DIR="$PWD/private/bayonetta3-shaders-repro/spirv" \\
  bash scripts/b3_play.sh
```

The helper requires Vulkan-Headers 1.4.357 or newer and checks each module's `table.cpp`. Its development default is Debug; the explicit trailing Release option above overrides that, while `RECOMP_OPT=2` optimizes generated game code. The shared build directory matches the player's default. The runner creates a separate mutable data root and links the preparation outputs and shader bundle. Preserve existing data and saves. Python Tk is needed only for the optional desktop launcher. [Playing and platform setup](PLAYING.md) covers Deck settings, runtime dependencies, Windows, Android, macOS, controls and troubleshooting; the Linux runner/GUI is not the macOS player entry point.

## Troubleshooting and privacy

An identity mismatch generally means the inputs do not match the configured version; do not bypass validation. If a test is absent or skipped, check the CMake conditions and optional tools. For a reproducible report, include the command, operating system, compiler and tool versions, sanitized error text, and exactly which step failed. Remove usernames, private paths, game data, generated sources, shaders, saves, keys and other sensitive material before sharing. See [CONTRIBUTING.md](CONTRIBUTING.md) and [LICENSING.md](LICENSING.md).
