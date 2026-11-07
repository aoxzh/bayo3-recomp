# Reproduce and play with a coding agent

This page is a task you can hand to a coding agent so it brings up **your own local build of the game**, rather than stopping once the unit tests pass. Multi-platform combat measurements and the Windows chapter-selection route are documented in [PERFORMANCE.md](PERFORMANCE.md). [PLAYING.md](PLAYING.md) is the platform launch guide, and [BUILD.md](BUILD.md) covers CPU preparation and the shader toolchain.

## Copy this task

```text
Help me build and run Bayonetta 3 Recomp on this machine using my own legally
obtained Bayonetta 3 version 1.2.0 files. Read AGENTS.md, my preferred-language
README, docs/PLAYING.md, docs/BUILD.md and docs/PERFORMANCE.md first.

Inspect the OS, CPU architecture, GPU/driver and installed tools. Locate inputs
I have already supplied; ask only for missing game/tool paths or actual choices.
Do not assume example paths exist. Use a per-step task list and continue through
normal interactive launch, not just the standalone unit tests.

1. Verify ExeFS main/subsdk0/sdk/main.npdm and the merged RomFS against the
   project configuration. Run CPU preparation/generation with the pinned
   requirements, in POSIX or native Windows Python (on Windows the RomFS/ExeFS
   links are directory junctions when symlinks are not permitted). A complete
   generation has been run on native Windows for both programs; generating on a
   POSIX host and transferring gen/ and analysis/ also works. Do not bypass version
   validation or output locks.
2. Build the external pinned shader dependencies using docs/BUILD.md. Run the
   synthetic worker/harness checks, then the full local extract/build/translate/
   verify sequence. Examine rejected translations, then run the separate local
   publish action. Set B3_SPIRV_DIR explicitly to that bundle's spirv directory.
3. Build a Release game executable for the target. For Linux, use build-deck
   consistently and RECOMP_OPT=2. For Windows use the prefixed LLVM-MinGW
   drivers and install libopus.dll next to the executable. For Android use the
   B3-only NativeActivity build and include libopus.so. For Mac configure first
   and use the direct executable path, not the headless diagnostic wrapper.
4. Assemble the target's runtime data root: analysis/, extract/romfs/, and
   spirv/results.tsv plus its modules. Verify that files are not nested one
   directory too deep. Install required platform codecs/drivers, and grant the
   Android app its required storage access when setting up my device.
5. Launch normally with sound, a visible window and input. Do not leave
   headless/mute, automatic input, frame dumps or diagnostic flags enabled.
   Verify the title/menu, input, sound and actual interactive scene. With my own
   appropriately progressed save, try Continue -> hub -> hold L and aim at a
   purple dartboard/map node -> A -> checkpoint -> A. Do not confuse this with
   the optional Records of Time book route, which starts program 1 (the
   Bayonetta Origins prologue); that route has not been played through yet.
6. If launch fails, use the troubleshooting checklist in docs/PLAYING.md and
   inspect the actual error: missing Opus, missing analysis module, inaccessible
   RomFS, missing shader table/modules, permissions and codec failures first.
   Keep working on concrete fixable setup problems. If required input/hardware
   is unavailable, identify exactly what is missing and finish independent work.

Keep game inputs, generated code/shaders, saves, keys, raw logs and path-bearing
manifests private. Never publish or upload them. Back up my existing save before
any migration; do not replace it with a downloaded save or delete unknown data.
Preserve licensing notices. Record what you changed and the exact commands.

Report unit tests, build, translation validation and interactive gameplay as
separate results. Compare performance only with matching device/scene/settings
and state what was actually tested on my machine. Existing project gameplay
results remain evidence even if this machine has not reproduced them; do not
call the project unplayable because one automation check fails. Do not claim a
complete playthrough: the reported Prologue transition issue remains.
```

## Stage outputs and completion checks

| Stage | Local output | Check before proceeding |
|---|---|---|
| CPU preparation | `private/bayonetta3/analysis`, `gen`, `extract` | All three module images and generated `table.cpp` files; version and identity validation succeeds |
| Shader pipeline | Work directory, then `private/bayonetta3-shaders-repro/spirv` | Verification succeeds; table and accepted SPIR-V modules exist; look at failures instead of suppressing them |
| Game build | Host executable or Android APK | Release build; necessary Opus runtime present; generated modules linked |
| Runtime data | Separate platform data root | Readable analysis, RomFS and shader paths; existing saves preserved |
| Normal launch | Local log and interactive display | Input, audio, menu and the actual scene checked; fresh-save path and progressed-save selection kept apart |

The Windows standalone baseline is 12 tests passing with one UBSan test skipped, as recorded in [BUILD.md](BUILD.md#windows-standalone-test-baseline). Tests are diagnostics; they are not the finish line when someone asked to play. During public-export preparation the full clean game pipeline was not rerun on every platform, so the dependency builds, synthetic tests, limited real shader check and existing platform gameplay measurements each cover a different scope.

## Source map

- [`recomp/`](../recomp): the static recompiler. [`runtime/`](../runtime): host services, input and audio, NVN/Vulkan rendering.
- [`games/bayonetta3/`](../games/bayonetta3) and [`config/bayonetta3-1.2.0.json`](../config/bayonetta3-1.2.0.json): game and version integration.
- [`tools/prepare_bayonetta3.py`](../tools/prepare_bayonetta3.py): POSIX CPU preparation. Even `--inspect-only` writes private analysis and a manifest containing paths.
- [`tools/bayonetta3_shaders.py`](../tools/bayonetta3_shaders.py): extraction, translator build, translation, validation and separate local publication.
- [`runtime/shaders/`](../runtime/shaders): reconstructed non-game presentation shaders. Read [licensing/provenance](LICENSING.md) before distributing them.
- [`tests/`](../tests): game-free component checks. Private game-image autodrive fixtures are omitted; the synthetic cases remain.
