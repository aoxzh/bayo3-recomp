# Gameplay status and measured performance

The measurements below come from development builds during October and November 2026, including changes that were still uncommitted in the working tree at the time. Combat has been reached on Windows, Steam Deck, Android and macOS, and Windows chapter selection has been driven from the hub dartboard into Chapter 8. None of this is a claim of a completed playthrough, and performance is not guaranteed to match on other devices.

Reports, raw logs, saves and local paths behind these numbers stay out of the repository. Hardware, driver versions and test conditions are kept, because a frame rate without them says nothing.

## Measurements

One row per device. Where a device was measured more than once, the results share the Conditions column instead of repeating the hardware.

| Platform and hardware | Result | Conditions | Date |
|---|---|---|---|
| Windows, Intel Core Ultra X7 358H with Arc B390 iGPU | Prologue combat: median **89.2 fps**, p10 **59.3**, p90 **105.5**. Light scenes reach **120**; a later, heavier fight ran around **57**.<br>Separate 1080p test at 60 Hz: median **59.7**, p10 **56.8**, p5 **54.8**, minimum **51.0** over 93 reporting windows.<br>Chapter 8: entry and gameplay windows **75–84 fps**, one archived window at **92.6** | 720p handheld environment at `NX_DISPLAY_HZ=120` for the first line; the 1080p line used `NX_RENDER_RES=1920x1080` at 60 Hz and is not a 1080p run at 89 fps. Chapter 8 was reached from a progressed save through the hub dartboard. Driver 32.0.101.9150, Vulkan 1.4, Release build | 2026-11-03 |
| Steam Deck, AMD Van Gogh, RADV | Prologue combat median **36.3–36.8**, p10 **30.2–34.4**, minimum **26.6 fps** | 720p handheld, 15 W, GPU fixed at 1600 MHz, uncapped; three runs, first texture-loading window excluded | 2026-11-01 |
| Android, Snapdragon 8 Gen 2 with Adreno 740 | Cold runs median **30.0**, minimum **29.1–29.4**. Warm late combat **27.3–28.6**, minimum **25.7–27.1 fps**.<br>Paced at 40: **40.0** for the first ~95 combat windows (about 2.4 minutes, minimum 38.2), then about **28** with `NX_GPU_TURBO=1` or about **33** without it once the GPU clock is limited | 720p handheld, default 30 fps cap, `NX_GPU_TURBO=1`; the warm numbers come from repeated runs at a battery temperature around 39 °C.<br>The 40 line: `NX_FPS_CAP=60 NX_DISPLAY_HZ=40`, started at a 33 °C battery, not charging; one run per arm | 2026-11-01<br>2026-11-07 |
| Android, Snapdragon 8 Elite (SM8750) with Adreno 830 | Prologue combat, uncapped, after the app was added to the system's game list: most windows **54–60 fps**, one dip to 40–51, over 3 minutes.<br>Before that, with the vendor's power manager holding the CPU clusters near 2.2 GHz (1.36 GHz at times): **24–58 fps**, swinging with the clock cap. The GPU was never the limit (fence wait 0) | `NX_FPS_CAP=60`, 120 Hz panel, `NX_GPU_TURBO=1`. The game-list run was **charging** (battery 35 → 38.7 °C), the two earlier runs were not, so they are not a clean comparison. Some vendors only lift CPU clocks for apps on their game list; outside it the replay thread is CPU-bound | 2026-11-07 |
| macOS, Apple M4 (10-core GPU), MoltenVK | Prologue combat median **18.3 / 18.9 / 18.9 fps** with chunk size 150; new default run **18.0**, older baseline **15.9** | 1600×900 reporting window, 700-second diagnostic runs at `NX_GPU_PROFILE=3`; combat windows selected by upload >50 MB and fps <30 | 2026-10-27 |

`p10` is the tenth percentile of reporting windows, not a frame-time-derived 1% low. These numbers come from different scenes, sampling methods and thermal states, so they are not directly comparable.

The Mac figures deserve a caveat beyond that: the filter drops windows at or above 30 fps and the probe itself costs frames. They describe an older instrumented run, not today's ceiling, and the ordinary visible-window player path on macOS still has not been measured separately.

### Steam Deck run to run

| Run | Median | p10 | Minimum |
|---|---:|---:|---:|
| 1 | 36.8 | 34.4 | 32.0 |
| 2 | 36.5 | 33.7 | 32.0 |
| 3 | 36.3 | 30.2 | 26.6 |

Three short combat runs. The first two covered less of the late fight; the third had five of its 75 windows below 30 fps. Cutscenes between fights can still drop frames, and nothing here establishes performance across a full chapter or a long session.

### What the Android numbers cover

The Snapdragon 8 Gen 2 / Adreno 740 device has three cold and three warm measurements, but only one cold run covered the whole late-fight section. Across the reported three Deck runs and five phone runs the corrected configuration produced no crash or GPU hang. Image correctness was checked against a limited set of screenshots and thumbnails, not frame by frame. Turnip was not tested. 

The 40 FPS runs of 2026-11-07 show where the limit is once the phone is warm. The replay thread needs about 17 ms per frame, and the GPU about 13.5 ms at full clock. The drop from 40 comes in a single reporting window when the GPU clock is limited: the replay time stays the same while the wait for the GPU grows. Buffer uploads and copies are a small part of the GPU time (0.44 ms of 13.5), and the game draws its scene passes at its own resolution whatever resolution the runtime reports, so neither is a lever for holding 40 on this device.

## What works, and what does not

- Boot, menus, cutscenes, audio, save loading and actual combat all have a working implementation and platform evidence behind them. Performance and rendering still differ from device to device.
- Logging is asynchronous: a background thread writes the log file, so a slow disk costs log lines, not frames. On Windows the old synchronous logger cost 5-13 ms per line and stalled a frame for 60-230 ms every 1-2 s; `NX_LOG_STDERR=1` restores the per-line stderr/logcat copy for debugging. The on-disk pipeline cache is saved the same way: the render thread snapshots it, one background thread writes the 12-20 MB file. The small `pipelines_*.keys` warm set is queued every 60 frames and flushed when the window closes, so the next launch prewarms the pipelines this session learned.
- The hub wall map doubles as a chapter selector: hold **L**, aim the left stick at a purple node, press **A**, pick a checkpoint, press **A** again. Chapter 8 was reached this way on Windows. You need your own save with appropriate progress; none is distributed.
- The **Records of Time book** is a separate route: it starts program 1, the Bayonetta Origins prologue. On Windows, program 1 builds from code generated on Windows, runs from its own RomFS (which the 1.2.0 update contains in full) and draws its first 3D scene with its own translated shaders; a headless 300-second run drew every draw without a crash. Starting it from the book in the game and returning to Bayonetta 3 has not been played through yet, and its image has not been compared with a reference. The dartboard selector does not need it.
- The normal story route still has a reported Prologue transition and loading problem. Reaching Chapter 8 through chapter selection says nothing about whether a fresh save can be played to the end.
- Windows testing here covers Arc B390 only, not other vendors. The macOS combat evidence is older than the Deck, Android and Windows reports, and diagnostic execution is not the same thing as a tested desktop player session.

## Reproducing a measurement

Get a normal launch working first by following [Playing and platform setup](PLAYING.md). Then record the game version, source revision or snapshot, compiler and build type, device and GPU, driver, display refresh rate, render resolution, cap, power and clock settings, thermal state, save or checkpoint, and test duration. Keep loading, cutscenes, hub and combat separate instead of pooling their frame rates. Your save and raw captures are yours to keep private; share sanitized aggregate numbers.

The repository includes combat and capture tools, but their heuristic classifications and missing fixtures are no substitute for looking at the scene. An automated `hub` test failing does not prove hub gameplay or chapter selection is broken: the earlier automation missed the dartboard interaction entirely, and the later manual test succeeded.
