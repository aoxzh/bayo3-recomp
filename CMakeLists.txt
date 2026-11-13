cmake_minimum_required(VERSION 3.25)
project(Bayonetta3Recomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The game this build is for: games/<NX_GAME>/ holds what only that game needs (game.cpp: nx/game.h), the shared
# runtime is the rest. One build directory per game.
set(NX_GAME "bayonetta3" CACHE STRING "Game to build (a folder under games/)")
set(NX_MODULES main sdk)
if(NX_GAME STREQUAL "bayonetta3")
    set(NX_MODULES main subsdk0 sdk)
    add_compile_definitions(NX_BAYONETTA3=1)
elseif(NX_GAME STREQUAL "bayonetta3prog1")
    # B3's second program (01004a4010fea001, what the hub's records book launches through ExecuteProgram
    # ProgramIndex 1). Same engine/runtime, but only two modules: it ships main + sdk, no subsdk0.
    add_compile_definitions(NX_BAYONETTA3=1 NX_BAYONETTA3_PROG1=1)
endif()
set(NX_GAME_LIBRARIES)
foreach(mod IN LISTS NX_MODULES)
    list(APPEND NX_GAME_LIBRARIES game_${mod})
endforeach()
set(NX_RECOMP_OPT "2" CACHE STRING "Generated code optimization level (0, 1, 2 or 3)")
if(NOT NX_RECOMP_OPT MATCHES "^[0-3]$")
    message(FATAL_ERROR "NX_RECOMP_OPT must be 0, 1, 2 or 3")
endif()
set(NX_GAME_DIR "${CMAKE_SOURCE_DIR}/games/${NX_GAME}")
if(NOT EXISTS "${NX_GAME_DIR}/game.cpp")
    message(FATAL_ERROR "games/${NX_GAME}/game.cpp not found")
endif()

# Generated code of the game (python -m recomp <nso> <gen dir>/<module> --name <module> --base ...). Never committed.
# Hades keeps the layout from before there were other games (private/gen); the others use private/<game>/gen.
if(NX_GAME STREQUAL "hades")
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/gen")
else()
    set(NX_GEN_DEFAULT "${CMAKE_SOURCE_DIR}/private/${NX_GAME}/gen")
endif()
set(NX_GEN_DIR "${NX_GEN_DEFAULT}" CACHE PATH "Recompiled module sources")

# Is the game target part of this build? The sanitizer options need the answer before any target exists: whole-process
# AddressSanitizer cannot coexist with the fixed guest layout, so it is refused when generated modules are present
# (cmake/NXSanitizers.cmake). CONFIGURE_DEPENDS: generating the tree later triggers a reconfigure and re-checks this.
file(GLOB NX_GEN_PROBE CONFIGURE_DEPENDS "${NX_GEN_DIR}/main/*.cpp")
if(NX_GEN_PROBE)
    set(NX_HAVE_GENERATED_CODE TRUE)
else()
    set(NX_HAVE_GENERATED_CODE FALSE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand -Wno-unused-function)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Unit tests of runtime pieces that build without the game (docs/nvn/WINDOWS-PLAN.md, "Test plan"): ctest.
option(NX_TESTS "Build the standalone unit tests" OFF)

# The movie-API probe (runtime/src/hle_movie.cpp): logs the movie:: entry points the game actually reaches and then
# calls the real implementation, so a probe build behaves like a normal one. It is a research switch, not a feature,
# and it is OFF by default because it perturbs the load path (BUGS §2.13). Previously the file was guarded by
# NX_MOVIE_PROBE_BUILD, which nothing ever defined — the probe could not be built at all.
option(NX_MOVIE_PROBE "Build the movie:: HLE probe (research; changes the load path, off by default)" OFF)
if(NX_MOVIE_PROBE)
    add_compile_definitions(NX_MOVIE_PROBE_BUILD=1)
endif()

# The movie:: vtable patch (runtime/src/hle_movie_api.cpp): rewrites the movie::NnCodec and movie::Extractor vtables in
# the loaded image so their virtual calls reach host code. The slot maps were read statically from the module image
# (docs/LINUX-VERIFICATION-2026-10-22.md §12), so this needs no probe build; NX_MOVIE_VTABLE=log installs a logging
# table that tail-calls the game's own implementation, which is how the seam gets verified before FFmpeg is wired in.
option(NX_MOVIE_VTABLE "Build the movie:: vtable patch (NX_MOVIE_VTABLE=log to trace the seam)" OFF)
# The frame-fetch observer, ON by default on desktop: it compiles the same file, and at runtime (NX_MOVIE_VTABLE unset)
# patches only movie::Decoder, at the first movie open, to observe the game's own frame fetch. That fetch names the film,
# its timestamp and its two plane buffers, so movie.cpp serves each plane from the right film at the game's own time
# instead of guessing by open order and wall clock (issue #3: the game pre-opens the next film while one plays).
# NX_MOVIE_VTABLE=0 turns it off at runtime; =log/1 is the full study mode. A separate option
# name, so build folders that cached NX_MOVIE_VTABLE=OFF pick it up on a Rebuild. Android: OFF until checked on a device.
if(ANDROID)
    option(NX_MOVIE_FRAME_SOURCE "Observe the game's movie frame fetch (movie::Decoder slot 15)" OFF)
else()
    option(NX_MOVIE_FRAME_SOURCE "Observe the game's movie frame fetch (movie::Decoder slot 15)" ON)
endif()
if(NX_MOVIE_FRAME_SOURCE)
    set(NX_MOVIE_VTABLE ON)
endif()
# Serving frames from FFmpeg through that table. Needs FFmpeg's development libraries; OFF until both movies play.
option(NX_MOVIE_FFMPEG "Serve movie:: from FFmpeg (implies NX_MOVIE_VTABLE)" OFF)
if(NX_MOVIE_FFMPEG)
    set(NX_MOVIE_VTABLE ON)
endif()
# The define is applied to the game executable target only (target_compile_definitions after the platform
# branches): a directory-scope add_compile_definitions also hits the 596 generated module translation units, which
# turns flipping this option into a recompile of the whole recompiled game.
if(NX_MOVIE_VTABLE)
    set(NX_MOVIE_VTABLE_DEFINE ON)
endif()

# Sanitizers and checked STL: options NX_SANITIZE, NX_SANITIZE_RECOVER, NX_SANITIZE_GAME, NX_ALLOW_ASAN_TESTS,
# NX_STL_DEBUG, NX_LLVM_ROOT, plus the configure-time sanitizer-runtime link probe. Included before any target so the
# flags cover every target defined below; all options default off (docs/SANITIZERS.md).
include(cmake/NXSanitizers.cmake)

if(NX_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(am_lifecycle_test tests/am_lifecycle_test.cpp)
    target_include_directories(am_lifecycle_test PRIVATE runtime/include runtime/src)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT MSVC)
        target_compile_options(am_lifecycle_test PRIVATE -march=x86-64-v3)
    endif()
    add_test(NAME am_lifecycle COMMAND am_lifecycle_test)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(account_service_test tests/account_service_test.cpp)
        target_compile_definitions(account_service_test PRIVATE NX_BAYONETTA3=1)
        target_include_directories(account_service_test PRIVATE runtime/include)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            target_compile_options(account_service_test PRIVATE -march=x86-64-v3)
        endif()
        add_test(NAME account_service COMMAND account_service_test)
    endif()
    add_executable(gpu_pick_test tests/gpu_pick_test.cpp runtime/src/nvn/nvn_gpu_pick.cpp)
    target_compile_definitions(gpu_pick_test PRIVATE NX_GPU_PICK_STANDALONE)
    target_include_directories(gpu_pick_test PRIVATE runtime/src/nvn)
    add_test(NAME gpu_pick COMMAND gpu_pick_test)
    add_executable(tiling_test tests/tiling_test.cpp runtime/src/nvn/nvn_tiling.cpp)
    target_include_directories(tiling_test PRIVATE runtime/src/nvn)
    add_test(NAME tiling COMMAND tiling_test --selftest)
    add_executable(astc_test tests/astc_test.cpp runtime/src/nvn/nvn_astc.cpp)
    target_include_directories(astc_test PRIVATE runtime/src/nvn)
    add_test(NAME astc COMMAND astc_test --selftest)
    add_executable(bc7_encode_test tests/bc7_encode_test.cpp runtime/src/nvn/nvn_bc7_encode.cpp)
    target_include_directories(bc7_encode_test PRIVATE runtime/src/nvn)
    add_test(NAME bc7_encode COMMAND bc7_encode_test)
    # The host semaphores svc_nv.cpp installs when the guest SDK's NvOsSemaphoreCreate fails (the fake nvdec's
    # NULL-handle crash, docs/MOVIE-EV0005.md §16.4). Header-only, so it needs no game/runtime link.
    add_executable(nv_os_sem_test tests/nv_os_sem_test.cpp)
    target_include_directories(nv_os_sem_test PRIVATE runtime/src)
    find_package(Threads REQUIRED)
    target_link_libraries(nv_os_sem_test PRIVATE Threads::Threads)
    add_test(NAME nv_os_sem COMMAND nv_os_sem_test)
    # The guest movie memory resource's ledger walk (docs/MOVIE-EV0005.md §21): header-only (<cstdint>/<string>), so
    # like nv_os_sem it needs no game or runtime link, and it is the part of the movie-memory diagnosis a GPU-free
    # machine can check. The walk reads guest memory in the game; this feeds it a synthetic intrusive list, so the
    # "cannot loop, cannot fault, cannot over-report" properties are ctest checks and not a lost evening.
    add_executable(movie_ledger_test tests/movie_ledger_test.cpp)
    target_include_directories(movie_ledger_test PRIVATE runtime/src)
    add_test(NAME movie_ledger COMMAND movie_ledger_test)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(audio_out_test tests/audio_out_test.cpp)
        target_compile_definitions(audio_out_test PRIVATE NX_BAYONETTA3)
        target_include_directories(audio_out_test PRIVATE runtime/include)
        target_link_libraries(audio_out_test PRIVATE Threads::Threads)
        foreach(mode IN ITEMS device sync timer reject)
            add_test(NAME audio_out_${mode} COMMAND audio_out_test ${mode})
        endforeach()
        find_package(Python3 QUIET COMPONENTS Interpreter)
        find_path(NX_TEST_VULKAN_INCLUDE vulkan/vulkan.h
            HINTS "$ENV{VULKAN_HEADERS}" "$ENV{VULKAN_SDK}/include"
                  "${CMAKE_SOURCE_DIR}/private/toolchains/Vulkan-Headers-1.4.357/include")
        if(Python3_Interpreter_FOUND)
            # CPU checks need neither Vulkan headers nor game artifacts.
            foreach(check IN ITEMS merge frame_parts astc_cache pad_config render_scale touch_zones)
                add_test(NAME bayonetta3_nvn_${check}
                    COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/bayonetta3_nvn_${check}_test.py")
            endforeach()
            foreach(check IN ITEMS b3_scene_clock movie_timing movie_frame_source movie_decoder_pipe b3_generation_hooks audio_playback)
                add_test(NAME ${check}
                    COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/${check}_test.py")
            endforeach()
            if(NX_HAVE_GENERATED_CODE AND CMAKE_GENERATOR STREQUAL "Ninja")
                add_test(NAME audio_sdl_playback
                    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/audio_sdl_playback_test.py"
                        --build-dir "${CMAKE_BINARY_DIR}")
                set_tests_properties(audio_sdl_playback PROPERTIES SKIP_RETURN_CODE 77)
            endif()
            if(EXISTS "${NX_GEN_DIR}/main/code_163.cpp" AND EXISTS "${NX_GEN_DIR}/main/code_235.cpp")
                add_test(NAME b3_guest_fastpaths
                    COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                        "B3_GEN_MAIN=${NX_GEN_DIR}/main"
                        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/b3_guest_fastpaths_test.py")
            endif()
            add_test(NAME nvn_astc_partition
                COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/nvn_astc_partition_test.py")
            add_test(NAME generated_math
                COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/generated_math_test.py")
            add_test(NAME neon_tbl
                COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/neon_tbl_test.py")
            set_tests_properties(neon_tbl PROPERTIES SKIP_RETURN_CODE 77)
            add_test(NAME nvn_draw_frame_stats
                COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/nvn_draw_frame_stats_test.py")
            add_test(NAME bayonetta3_hid
                COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/bayonetta3_hid_abi.py")
        endif()
        if(Python3_Interpreter_FOUND AND NX_TEST_VULKAN_INCLUDE)
            # Exercise the production SDL/HID path using virtual controllers when SDL3 is available.
            add_test(NAME nvn_sdl_gamepad
                COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                    "VULKAN_HEADERS=${NX_TEST_VULKAN_INCLUDE}"
                    "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/nvn_sdl_gamepad_test.py")
            set_tests_properties(nvn_sdl_gamepad PROPERTIES SKIP_RETURN_CODE 77)
            foreach(check IN ITEMS command buffer_copy pipeline host_consumers stage_binding zcull indirect fence alpha program record_batch dedup image_retire)
                add_test(NAME bayonetta3_nvn_${check}
                    COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                        "VULKAN_HEADERS=${NX_TEST_VULKAN_INCLUDE}"
                        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/bayonetta3_nvn_${check}_test.py")
            endforeach()
            foreach(check IN ITEMS texture_span descriptor_templates descriptor_resolve replay_fence_poll compute_keep writeback_pool sync_submit host_import astc_upload)
                add_test(NAME nvn_${check}
                    COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
                        "VULKAN_HEADERS=${NX_TEST_VULKAN_INCLUDE}"
                        "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/nvn_${check}_test.py")
            endforeach()
        endif()
    endif()
    # Cross-platform entry-point contract and synthetic acceptance records in a
    # scratch repository. These Python dry-run tests need no GPU, build or game data.
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME cross_platform_harness
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_cross_platform_harness.py")
        # Register only synthetic driver tests. The game-derived saved-frame
        # fixtures are excluded from this export and their tests are not run.
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import PIL"
            RESULT_VARIABLE NX_TEST_PILLOW_STATUS OUTPUT_QUIET ERROR_QUIET)
        if(NX_TEST_PILLOW_STATUS STREQUAL "0")
            add_test(NAME b3_autodrive_synthetic
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_b3_autodrive.py" SyntheticRegressions)
        endif()
        find_program(NX_BASH bash)
        if(NX_BASH)
            add_test(NAME b3_battle_parser
                COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_b3_battle.py")
        endif()
        # The launcher's settings model (private/b3.conf): no Tk, no display, no game, so it runs everywhere.
        add_test(NAME b3_launcher_config
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_b3_launcher_config.py")
        # tools/b3_win.py (the Windows setup and runner): settings rules, dump layouts, links, the program switch.
        add_test(NAME b3_win
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_b3_win.py")
        # Mock SDL controller lifecycles without opening hardware or a Tk window.
        add_test(NAME b3_launcher_sdl
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_b3_launcher_sdl.py")
    endif()
    add_executable(shader_cache_test tests/shader_cache_test.cpp runtime/src/nvn/nvn_shaders.cpp
                   runtime/src/nvn/nvn_spirv_reflect.cpp)
    target_compile_definitions(shader_cache_test PRIVATE NX_SHADERS_STANDALONE)
    target_include_directories(shader_cache_test PRIVATE runtime/src/nvn)
    if(EXISTS "${CMAKE_SOURCE_DIR}/private/wonder/spirv")  # the game's own shaders: never in the repository
        add_test(NAME shader_cache COMMAND shader_cache_test "${CMAKE_SOURCE_DIR}/private/wonder")
    endif()
    if(WIN32)
        add_executable(memguard_test tests/memguard_test.cpp)
        target_link_libraries(memguard_test PRIVATE psapi)
        add_test(NAME memguard COMMAND memguard_test)
    endif()

    # Deliberately-UB smoke check for the sanitizer toolchain (scripts/ubsan_smoke.sh, docs/SANITIZERS.md). Registered
    # in every NX_TESTS build, but the script SKIPS itself (ctest return code 77) when this build exported no UBSan
    # flags, so a normal test run never compiles or runs the UB program.
    find_program(NX_BASH bash)
    if(NX_BASH)
        add_test(NAME ubsan_smoke COMMAND "${NX_BASH}" "${CMAKE_SOURCE_DIR}/scripts/ubsan_smoke.sh"
                 --build-dir "${CMAKE_BINARY_DIR}")
        set_tests_properties(ubsan_smoke PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # Symbols for every sanitizer report: UBSan prints addresses only unless the runtime has a symbolizer.
    # halt_on_error=1: a report in a test log must not be a one-line warning that scrolls past.
    if(NX_SANITIZE_ACTIVE)
        get_property(NX_TEST_NAMES DIRECTORY PROPERTY TESTS)
        if(NX_TEST_NAMES)
            set(NX_TEST_SANITIZER_ENVIRONMENT "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
            if("address" IN_LIST NX_SANITIZE_LIST)
                list(APPEND NX_TEST_SANITIZER_ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1")
            endif()
            set_tests_properties(${NX_TEST_NAMES} PROPERTIES ENVIRONMENT "${NX_TEST_SANITIZER_ENVIRONMENT}")
        endif()
    endif()
endif()

# Without the generated code (a checkout with no game data) only the tests above are built.
if(NOT NX_HAVE_GENERATED_CODE)
    message(WARNING "No generated code in ${NX_GEN_DIR}/main: building the unit tests only")
    return()
endif()

# Generated code normally carries no debug info (the objects are large and the guest instruction is already visible in
# the source comments). NX_GEN_LINE_TABLES=ON adds DWARF line tables to the generated modules only, so a host PC inside
# a generated function resolves to the exact code_*.cpp line (which contains the "NXB + 0x..." literal and the guest
# disassembly comment) with atos/llvm-symbolizer and tools/symbolize.py (docs/CRASH-REPORTS.md). Off by default, so
# default builds keep their current flags exactly.
option(NX_GEN_LINE_TABLES
    "Give the generated game modules DWARF line tables (-gline-tables-only, or -g1 with GCC) for symbolization" OFF)
set(NX_GEN_LINE_TABLES_FLAG "")
if(NX_GEN_LINE_TABLES AND NOT (NX_SANITIZE_ACTIVE AND NX_SANITIZE_GAME))
    # A sanitized-game build already gets full -g through the sanitizer flags; -gline-tables-only after it would strip
    # variable info UBSan can use, so in that combination the full -g wins. Never add two debug flags.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(NX_GEN_LINE_TABLES_FLAG "-gline-tables-only")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(NX_GEN_LINE_TABLES_FLAG "-g1")
    else()
        message(WARNING "NX_GEN_LINE_TABLES is not supported with ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}: "
                        "generated modules keep no line tables (Clang and GCC are supported).")
    endif()
endif()

# Generated code on Linux is compiled without debug info to keep the objects small. NX_GEN_LINE_TABLES replaces the
# Linux -g0 with line tables; a sanitized-game build (NX_SANITIZE_GAME=ON) needs full -g, or its reports point at nothing.
set(NX_GEN_DEBUG_FLAG "-g0")
if(NX_GEN_LINE_TABLES_FLAG)
    set(NX_GEN_DEBUG_FLAG "${NX_GEN_LINE_TABLES_FLAG}")
elseif(NX_SANITIZE_ACTIVE AND NX_SANITIZE_GAME)
    set(NX_GEN_DEBUG_FLAG "-g")
endif()

foreach(mod IN LISTS NX_MODULES)
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    if(NOT EXISTS "${NX_GEN_DIR}/${mod}/table.cpp" OR NOT src)
        message(FATAL_ERROR "Missing generated module ${mod}; run tools/prepare_bayonetta3.py --generate")
    endif()
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
    if(ANDROID)
        # Hidden visibility: calls between recompiled functions are direct, not through the PLT.
        # armv8.2-a+crypto: cpu.h's AES/SHA paths need __ARM_FEATURE_AES/SHA2 (the NDK default armv8-a has neither).
        target_compile_options(game_${mod} PRIVATE -O${NX_RECOMP_OPT} -fPIC -fvisibility=hidden -fno-semantic-interposition
                               -march=armv8.2-a+crypto ${NX_GEN_LINE_TABLES_FLAG})
    elseif(APPLE)
        target_compile_options(game_${mod} PRIVATE -O${NX_RECOMP_OPT} -fvisibility=hidden ${NX_GEN_LINE_TABLES_FLAG})
    elseif(UNIX)
        # Linux x86-64: AVX2/FMA/BMI as the Windows build (/arch:AVX2); no FMA contraction, so a guest fmul + fadd
        # rounds twice, as on the Switch. Guest FP instructions have no host libc errno side effect; allow hardware
        # sqrt without an errno-only libm fallback. This does not enable fast-math or change the runtime's flags.
        target_compile_options(game_${mod} PRIVATE -O${NX_RECOMP_OPT} ${NX_GEN_DEBUG_FLAG} -fvisibility=hidden
                               -march=x86-64-v3 -ffp-contract=off -fno-math-errno)
    elseif(NX_GEN_LINE_TABLES_FLAG)
        # Windows: /Z7 embeds CodeView debug info (with line tables) in the object; MSVC and clang-cl both accept it.
        target_compile_options(game_${mod} PRIVATE /Z7)
    endif()
    if(NX_SANITIZE_ACTIVE AND NOT NX_SANITIZE_GAME AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # The runtime, game glue and tests were given -fsanitize=... by cmake/NXSanitizers.cmake; the generated and
        # recompiled translation units are left uninstrumented unless NX_SANITIZE_GAME=ON. They are 690 TUs and guest
        # code expresses wrapping arithmetic as host C++ only by convention, so instrumenting them by default would be
        # slow and noisy. -fno-sanitize=all cancels the directory-level flags for this target only.
        target_compile_options(game_${mod} PRIVATE -fno-sanitize=all)
    endif()
endforeach()

set(NX_RUNTIME_SOURCES
    runtime/src/main.cpp
    runtime/src/sampler.cpp
    runtime/src/loader.cpp
    runtime/src/memory.cpp
    runtime/src/kernel.cpp
    runtime/src/kernel_watch.cpp
    runtime/src/ipc.cpp
    runtime/src/svc_am.cpp
    runtime/src/svc_set.cpp
    runtime/src/svc_misc.cpp
    runtime/src/svc_nv.cpp
    runtime/src/svc_time.cpp
    runtime/src/svc_process.cpp
    runtime/src/hle_fs.cpp
    runtime/src/svc_account.cpp
    runtime/src/users.cpp
    runtime/src/hle_ro.cpp
    runtime/src/hle_vi.cpp
    runtime/src/hle_hid.cpp
    runtime/src/hle_net.cpp
    runtime/src/hle_audio.cpp
    runtime/src/hle_pl.cpp
    runtime/src/input_script.cpp
    runtime/src/touch_layout.cpp
    runtime/src/touch_art.cpp
    runtime/src/perf_hint.cpp
    runtime/src/frame_stats.cpp
    runtime/src/write_watch.cpp
    runtime/src/nvn/nvn_caps.cpp
    runtime/src/nvn/nvn_renderpass.cpp
    runtime/src/nvn/nvn_gpu_busy.cpp
    runtime/src/nvn/nvn_janitor.cpp
    runtime/src/nvn/nvn_trace.cpp
    runtime/src/nvn/nvn_device.cpp
    runtime/src/nvn/nvn_objects.cpp
    runtime/src/nvn/nvn_incarnation.cpp
    runtime/src/nvn/nvn_texture_layout.cpp
    runtime/src/nvn/nvn_program.cpp
    runtime/src/nvn/nvn_queue.cpp
    runtime/src/nvn/nvn_cmd.cpp
    runtime/src/nvn/nvn_cmd_replay.cpp
    runtime/src/nvn/nvn_state.cpp
    runtime/src/nvn/nvn_format.cpp
    runtime/src/nvn/nvn_enums.cpp
    runtime/src/nvn/nvn_sampler.cpp
    runtime/src/nvn/nvn_sampler_objects.cpp
    runtime/src/nvn/nvn_draw_state.cpp
    runtime/src/nvn/nvn_raster.cpp
    runtime/src/nvn/nvn_worker.cpp
    runtime/src/nvn/nvn_shaders.cpp
    runtime/src/nvn/nvn_spirv_reflect.cpp
    runtime/src/nvn/nvn_tiling.cpp
    runtime/src/nvn/nvn_vk.cpp
    runtime/src/nvn/nvn_gpu_pick.cpp
    runtime/src/nvn/nvn_image.cpp
    runtime/src/nvn/nvn_frames.cpp
    runtime/src/nvn/nvn_dedup.cpp
    runtime/src/nvn/nvn_written.cpp
    runtime/src/nvn/nvn_replay.cpp
    runtime/src/nvn/nvn_debug.cpp
    runtime/src/nvn/nvn_binds.cpp
    runtime/src/nvn/nvn_copy.cpp
    runtime/src/nvn/nvn_buffer_copy.cpp
    runtime/src/nvn/nvn_zcull.cpp
    runtime/src/nvn/nvn_indirect.cpp
    runtime/src/nvn/nvn_pipeline.cpp
    runtime/src/nvn/nvn_dummy.cpp
    runtime/src/nvn/nvn_stencil.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_upload.cpp
    runtime/src/nvn/nvn_astc.cpp
    runtime/src/nvn/nvn_astc_bc7.cpp
    runtime/src/nvn/nvn_bc7_encode.cpp
    runtime/src/nvn/nvn_spirv_patch.cpp
    runtime/src/nvn/nvn_spirv_clip.cpp
    runtime/src/nvn/nvn_spirv_alpha.cpp
    runtime/src/nvn/nvn_ultrawide.cpp
    runtime/src/nvn/nvn_present.cpp
    runtime/src/nvn/nvn_frame_overlay.cpp
    runtime/src/nvn/nvn_pipeline_cache.cpp
    runtime/src/nvn/nvn_bc.cpp
    runtime/src/nvn/nvn_draw.cpp
    runtime/src/nvn/nvn_sets.cpp
    runtime/src/nvn/nvn_clears.cpp
    runtime/src/nvn/nvn_mask_clear.cpp
    runtime/src/nvn/nvn_draw_trace.cpp
    runtime/src/vk_bridge.cpp
    runtime/src/vk_swapchain.cpp
    runtime/src/vk_gen.cpp
    runtime/src/vk_present.cpp
    runtime/src/code_preload.cpp
    runtime/src/settings_live.cpp
)
if(NX_GAME STREQUAL "bayonetta3" OR NX_GAME STREQUAL "bayonetta3prog1")
    list(APPEND NX_RUNTIME_SOURCES runtime/src/hle_fiber.cpp runtime/src/svc_opus.cpp runtime/src/movie.cpp
         runtime/src/hle_movie.cpp runtime/src/b3_scene_clock.cpp runtime/src/b3_fps_counters.cpp
         runtime/src/b3_model_lod.cpp
         runtime/src/b3_guest_fastpaths.cpp)
    # The movie:: vtable seam (docs/LINUX-VERIFICATION-2026-10-22.md §8): by default only the frame-fetch observer
    # (see the NX_MOVIE_VTABLE option above).
    if(NX_MOVIE_VTABLE)
        list(APPEND NX_RUNTIME_SOURCES runtime/src/hle_movie_api.cpp)
    endif()
endif()
file(GLOB NX_GAME_SOURCES CONFIGURE_DEPENDS "${NX_GAME_DIR}/*.cpp")
list(APPEND NX_RUNTIME_SOURCES ${NX_GAME_SOURCES})

# Vulkan headers (the same registry version tools/gen_vk.py generated from). The loader comes with the GPU driver.
if(DEFINED ENV{VULKAN_SDK})
    set(NX_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK")
else()
    set(NX_VULKAN_SDK "C:/VulkanSDK/1.4.357.0" CACHE PATH "Vulkan SDK")
endif()

if(ANDROID)
    # NativeActivity app: lib<game>.so, entry android_main (android_native_app_glue).
    set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(${NX_GAME} SHARED ${NX_RUNTIME_SOURCES} runtime/src/platform_android.cpp
        runtime/src/platform_android_input.cpp runtime/src/platform_android_audio.cpp runtime/src/memguard_android.cpp
        runtime/src/movie_android.cpp
        "${GLUE}/android_native_app_glue.c")
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS "${NX_VULKAN_SDK}/include" "${NX_VULKAN_SDK}/Include")
    target_include_directories(${NX_GAME} BEFORE PRIVATE "${NX_VULKAN_INCLUDE}")
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${GLUE}")
    # Our ANativeActivity_onCreate (platform_android.cpp) runs first, on the UI thread, then calls the glue's.
    set_source_files_properties("${GLUE}/android_native_app_glue.c" PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    # mediandk is the phone's own hardware video decoder: the Android stand-in for the host ffmpeg fork in
    # runtime/src/movie.cpp (a cutscene cannot shell out to ffmpeg inside an APK).
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBRARIES} android log aaudio mediandk)
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -fno-semantic-interposition -march=armv8.2-a+crypto)
    # The glue's ANativeActivity_onCreate must be exported. 16 KB page alignment: Android 15+ devices with 16 KB pages.
    target_link_options(${NX_GAME} PRIVATE -u ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
elseif(APPLE)
    # macOS: SDL3 (window, input, audio) and the Vulkan loader + MoltenVK from Homebrew
    # (brew install sdl3 vulkan-headers vulkan-loader molten-vk).
    find_package(SDL3 REQUIRED CONFIG)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS /opt/homebrew/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_mac.cpp runtime/src/memguard_mac.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBRARIES} SDL3::SDL3)
    # screen_refresh_hz() asks CoreGraphics for the display's mode.
    find_library(NX_COREGRAPHICS CoreGraphics)
    if(NX_COREGRAPHICS)
        target_link_libraries(${NX_GAME} PRIVATE "${NX_COREGRAPHICS}")
    endif()
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden)
    # The executable loads near 0x1_0000_0000, below the guest ranges (0x40_0000_0000 and up). The game runs on its
    # own thread with a 64 MiB stack (platform_mac.cpp), so the main thread's stack size does not matter.
elseif(UNIX)
    # Linux: SDL3 (window, input, audio) and the system Vulkan loader, as on macOS. SDL3 from the distribution or a
    # local build (-DCMAKE_PREFIX_PATH=<its install prefix>).
    find_package(SDL3 REQUIRED CONFIG)
    find_package(Threads REQUIRED)
    find_path(NX_VULKAN_INCLUDE vulkan/vulkan.h HINTS "${NX_VULKAN_SDK}/include" /usr/include /usr/local/include)
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_linux.cpp runtime/src/memguard_linux.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_INCLUDE}")
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBRARIES} SDL3::SDL3 Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_options(${NX_GAME} PRIVATE -fvisibility=hidden -march=x86-64-v3 -ffp-contract=off)
    # The executable (PIE) loads near 0x55_5555_0000_0000, far above the guest ranges (0x40_0000_0000 and up). The
    # game runs on its own thread with a 64 MiB stack (platform_linux.cpp).
else()
    add_executable(${NX_GAME} ${NX_RUNTIME_SOURCES} runtime/src/platform_win.cpp runtime/src/memguard_win.cpp
        runtime/src/movie_win.cpp)
    target_include_directories(${NX_GAME} PRIVATE runtime/include runtime/src "${NX_GAME_DIR}" "${NX_VULKAN_SDK}/Include")
    # XAudio2's import library is versioned per toolkit: MSVC ships `xaudio2.lib` (a forwarder to XAudio2_9), while
    # MinGW ships `libxaudio2_8.a` / `libxaudio2_9.a` and has no plain `xaudio2` at all. Both are named directly
    # (not through find_library) so the compiler's own library directory is what resolves them, exactly as it does
    # for `xinput` on the same line.
    if(MSVC)
        set(NX_XAUDIO2_LIB xaudio2)
    else()
        set(NX_XAUDIO2_LIB xaudio2_9)
    endif()
    # mfplat/mfreadwrite/mfuuid are Media Foundation: the cutscene decoder (runtime/src/movie_win.cpp) uses
    # IMFSourceReader rather than forking ffmpeg, which does not exist on Windows. winmm is timeBeginPeriod, the 1 ms
    # scheduler tick the vsync clock needs (platform_win.cpp raise_timer_resolution).
    target_link_libraries(${NX_GAME} PRIVATE ${NX_GAME_LIBRARIES} xinput ${NX_XAUDIO2_LIB} ole32 psapi
        mfplat mfreadwrite mfuuid winmm)
    # The runtime uses the portable C library (getenv, fopen, ...) on every host: no "_s" variants.
    target_compile_definitions(${NX_GAME} PRIVATE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    if(MSVC)
        # Keep host allocations low so the guest ranges (0x40_0000_0000..0x70_0000_0000) are always free; a big
        # main-thread stack (recompiled calls nest deeply); a link map for tools/symbolize.py.
        target_link_options(${NX_GAME} PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO /MAP)
    else()
        # MinGW/clang-cl equivalents. High-entropy ASLR spreads the image across the whole 64-bit space, which lands
        # the executable on top of one of the fixed guest reservations -- measured here: "cannot reserve alias at
        # 5800000000 (+1000000000): the range is in use" at the first run. Turning it off keeps the image in the low
        # 4 GiB, exactly what /HIGHENTROPYVA:NO does for the MSVC build. The 64 MiB main-thread stack is the other
        # half: the recompiled calls nest deeply and the MinGW default (1 MiB) overflows in the first scene.
        target_link_options(${NX_GAME} PRIVATE -Wl,--disable-high-entropy-va -Wl,--stack,67108864)
    endif()
endif()

# The movie:: vtable patch (NX_MOVIE_VTABLE): only the executable's own runtime/glue translation units see the define
# (runtime/src/main.cpp, movie.cpp use it; hle_movie_api.cpp is only in the source list when the option is on).
if(NX_MOVIE_VTABLE_DEFINE)
    target_compile_definitions(${NX_GAME} PRIVATE NX_MOVIE_VTABLE=1)
endif()

# NX_GEN_LINE_TABLES also gives the game executable's own translation units (runtime + game glue) line tables. On macOS
# the linker only exposes DWARF from static-archive members (the generated game_*.a) when the executable's own objects
# carry debug info too; without this atos resolves the main_XXXXXXXX symbol but not its code_*.cpp:<line>. As a bonus,
# runtime frames in crash reports get file:line as well. (ELF keeps archive .debug_line without this, but the extra
# line info is small and useful there too.)
if(NX_GEN_LINE_TABLES_FLAG)
    target_compile_options(${NX_GAME} PRIVATE ${NX_GEN_LINE_TABLES_FLAG})
endif()
