# Sanitizer and checked-standard-library support for host code.
#
# Included from CMakeLists.txt before any target is created, so the options below cover every target defined
# afterwards (unit tests, the runtime and, when requested, the generated game modules). User documentation:
# docs/SANITIZERS.md.
#
# Three layers, cheapest first (docs/SANITIZERS.md explains when to use which):
#   1. UBSan (-fsanitize=undefined): undefined behaviour in host/runtime code. NX_SANITIZE.
#   2. Checked STL (NX_STL_DEBUG): libstdc++ debug containers / libc++ hardening. Catches iterator and bounds abuse
#      UBSan cannot see.
#   3. The runtime's own checks: nx::fatal() (runtime/include/nx/runtime.h) and static_asserts. Always on.
#
# Everything defaults off: a default Release build is byte-for-byte unaffected.

include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

# ---- options ---------------------------------------------------------------------------------------------------

set(NX_SANITIZE "" CACHE STRING
    "Sanitizers for host code: 'undefined', 'address', a comma/semicolon list, or empty/none for off")
option(NX_SANITIZE_RECOVER "Continue after a sanitizer report instead of aborting on the first one" OFF)
option(NX_SANITIZE_GAME "Instrument the generated/recompiled game modules too (slow; guest code may rely on wrapping arithmetic)" OFF)
option(NX_STL_DEBUG "Checked standard-library containers: libstdc++ debug mode / libc++ hardening (docs/SANITIZERS.md)" OFF)
option(NX_ALLOW_ASAN_TESTS
    "Allow NX_SANITIZE=address for unit-test-only builds (no generated modules); refuses the game target" OFF)
set(NX_LLVM_ROOT "" CACHE PATH
    "LLVM installation that provides compiler-rt sanitizer runtimes (hint; configure with <root>/bin/clang++ to use it)")

if(NOT DEFINED NX_HAVE_GENERATED_CODE)
    message(FATAL_ERROR
        "cmake/NXSanitizers.cmake must be included from CMakeLists.txt after NX_HAVE_GENERATED_CODE is set.")
endif()

# ---- parse NX_SANITIZE -----------------------------------------------------------------------------------------

set(NX_SANITIZE_ACTIVE FALSE)
set(NX_SANITIZE_LIST)
if(NX_SANITIZE)
    string(REPLACE "," ";" _nx_sanitize_requested "${NX_SANITIZE}")
    foreach(_nx_san IN LISTS _nx_sanitize_requested)
        string(STRIP "${_nx_san}" _nx_san)
        if(_nx_san STREQUAL "" OR _nx_san STREQUAL "none")
            continue()
        endif()
        if(NOT _nx_san MATCHES "^(undefined|address)$")
            message(FATAL_ERROR
                "NX_SANITIZE: unsupported sanitizer '${_nx_san}'. Supported: undefined, address "
                "(comma or semicolon separated, e.g. -DNX_SANITIZE=undefined). See docs/SANITIZERS.md.")
        endif()
        if(NOT _nx_san IN_LIST NX_SANITIZE_LIST)
            list(APPEND NX_SANITIZE_LIST "${_nx_san}")
        endif()
    endforeach()
endif()

if(NX_SANITIZE_GAME AND NOT NX_SANITIZE_LIST)
    message(WARNING "NX_SANITIZE_GAME is ON but NX_SANITIZE is empty: there is nothing to instrument.")
endif()

# ---- compiler-rt discovery helpers -----------------------------------------------------------------------------

# Directory clang reports for compiler-rt ("$(clang++ --print-runtime-dir)"), empty when the compiler has no such
# concept (GCC, MSVC).
function(_nx_sanitizer_runtime_dir out_var compiler)
    execute_process(COMMAND "${compiler}" --print-runtime-dir
                    RESULT_VARIABLE _nx_rt_result
                    OUTPUT_VARIABLE _nx_rt_out
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_nx_rt_result EQUAL 0 AND _nx_rt_out AND IS_DIRECTORY "${_nx_rt_out}")
        set(${out_var} "${_nx_rt_out}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# Basenames of the compiler-rt runtime files for the requested sanitizers that live in <dir>.
function(_nx_sanitizer_runtime_libs out_var dir)
    if(NOT dir OR NOT IS_DIRECTORY "${dir}")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    set(_nx_patterns)
    if("undefined" IN_LIST NX_SANITIZE_LIST)
        list(APPEND _nx_patterns "${dir}/libclang_rt.*ubsan*")
    endif()
    if("address" IN_LIST NX_SANITIZE_LIST)
        list(APPEND _nx_patterns "${dir}/libclang_rt.*asan*")
    endif()
    file(GLOB _nx_libs ${_nx_patterns})
    set(_nx_names)
    foreach(_nx_lib IN LISTS _nx_libs)
        get_filename_component(_nx_name "${_nx_lib}" NAME)
        list(APPEND _nx_names "${_nx_name}")
    endforeach()
    list(JOIN _nx_names ", " _nx_names)
    set(${out_var} "${_nx_names}" PARENT_SCOPE)
endfunction()

# Re-run the exact link that failed by hand and return its output (tail), so the FATAL_ERROR can show the real
# linker diagnostic instead of just "try_compile failed". <flags> are the configure-time sanitizer flags.
function(_nx_sanitizer_link_log out_var)
    set(_nx_dir "${CMAKE_BINARY_DIR}/CMakeFiles/nx_sanitizer_probe")
    file(MAKE_DIRECTORY "${_nx_dir}")
    file(WRITE "${_nx_dir}/probe.cpp" "int main() { return 0; }\n")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${ARGN} "${_nx_dir}/probe.cpp" -o "${_nx_dir}/probe"
                    RESULT_VARIABLE _nx_result
                    OUTPUT_VARIABLE _nx_out
                    ERROR_VARIABLE _nx_err)
    set(_nx_log "${_nx_err}${_nx_out}")
    string(STRIP "${_nx_log}" _nx_log)
    string(LENGTH "${_nx_log}" _nx_len)
    if(_nx_len GREATER 1400)
        math(EXPR _nx_start "${_nx_len} - 1400")
        string(SUBSTRING "${_nx_log}" ${_nx_start} -1 _nx_log)
        set(_nx_log "...${_nx_log}")
    endif()
    set(${out_var} "${_nx_log}" PARENT_SCOPE)
endfunction()

# ---- whole-process ASan cannot coexist with the recompiled game modules ---------------------------------------

if("address" IN_LIST NX_SANITIZE_LIST)
    if(NX_HAVE_GENERATED_CODE)
        message(FATAL_ERROR
"NX_SANITIZE=address cannot be used for the game target: whole-process AddressSanitizer cannot coexist with the "
"guest address space.

  Recompiled guest modules are mapped at a fixed NX_BASE (0x7bcff00000 on macOS, 0x4000000000 on other hosts) with
  a shared 512 MiB reservation (runtime/include/nx/cpu.h, nx/runtime.h). ASan reserves shadow memory at fixed
  addresses -- its high shadow starts around 0x87fff8000, and the low shadow sits right below the guest ranges --
  so mapping the guest modules overlaps the shadow ASan needs. The process would die at startup (or silently corrupt
  memory), which is why this configuration is refused at configure time instead of linking a binary that cannot run.

  ASan is therefore only accepted for unit-test-only builds (no generated modules, nothing that maps the guest
  layout):
      cmake -S . -B <dir> -G Ninja -DNX_TESTS=ON -DNX_GEN_DIR=<empty dir> -DNX_SANITIZE=address
  UndefinedBehaviorSanitizer has no fixed shadow and IS supported for the full game target:
      cmake -S . -B <dir> -G Ninja -DNX_SANITIZE=undefined
  See docs/SANITIZERS.md (\"AddressSanitizer and the guest layout\").")
    elseif(NOT NX_TESTS)
        message(FATAL_ERROR
            "NX_SANITIZE=address is only accepted for unit-test-only builds (NX_TESTS=ON and no generated modules), "
            "and this configuration would build nothing. Add -DNX_TESTS=ON, or drop NX_SANITIZE=address. "
            "See docs/SANITIZERS.md.")
    elseif(NOT NX_ALLOW_ASAN_TESTS)
        message(FATAL_ERROR
"NX_SANITIZE=address with a unit-test-only build needs an explicit opt-in: add -DNX_ALLOW_ASAN_TESTS=ON.

  AddressSanitizer cannot be the default sanitizer combination here because it can never cover the game target:
  the recompiled guest modules are mapped at a fixed NX_BASE (0x7bcff00000 on macOS, 0x4000000000 elsewhere) and
  ASan's shadow memory starts around 0x87fff8000, so the guest reservation overlaps it. Allowing ASan silently
  would invite configuring it for a build that then refuses the game. As a tests-only aid it works for the
  standalone unit tests, which never map the guest layout.

  Accepted here (no generated modules in ${NX_GEN_DIR}):
      cmake -S . -B <dir> -G Ninja -DNX_TESTS=ON -DNX_GEN_DIR=<empty dir> \
            -DNX_SANITIZE=address -DNX_ALLOW_ASAN_TESTS=ON
  For the game target use -DNX_SANITIZE=undefined instead. See docs/SANITIZERS.md.")
    endif()
endif()

# ---- NX_STL_DEBUG: checked standard-library containers --------------------------------------------------------

if(NX_STL_DEBUG)
    # Which standard library is in use? Both probes compile against the current compiler and stdlib.
    check_cxx_source_compiles("
#include <vector>
#if !defined(__GLIBCXX__)
#  error not libstdc++
#endif
int main() { return 0; }
" NX_STL_IS_LIBSTDCXX)
    check_cxx_source_compiles("
#include <vector>
#if !defined(_LIBCPP_VERSION)
#  error not libc++
#endif
int main() { return 0; }
" NX_STL_IS_LIBCXX)

    if(NX_STL_IS_LIBSTDCXX)
        # Debug containers change the types' layout and iterators, so this definition must cover every translation
        # unit of a binary; add_compile_definitions does that for all targets defined after this include.
        message(STATUS "NX_STL_DEBUG: libstdc++ debug containers on (_GLIBCXX_DEBUG + _GLIBCXX_DEBUG_PEDANTIC)")
        add_compile_definitions(_GLIBCXX_DEBUG=1 _GLIBCXX_DEBUG_PEDANTIC=1)
    elseif(NX_STL_IS_LIBCXX)
        # libc++ 18+ hardening mode: a supported, ABI-stable debug/assertions mode. Probe that the headers actually
        # apply the requested mode instead of assuming the flag is understood.
        set(_nx_saved_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
        set(CMAKE_REQUIRED_DEFINITIONS "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG")
        check_cxx_source_compiles("
#include <vector>
#if !defined(_LIBCPP_HARDENING_MODE) || _LIBCPP_HARDENING_MODE != _LIBCPP_HARDENING_MODE_DEBUG
#  error libc++ did not apply the requested hardening mode
#endif
int main() { std::vector<int> v; return static_cast<int>(v.size()); }
" NX_STL_LIBCXX_HARDENING)
        if(NX_STL_LIBCXX_HARDENING)
            message(STATUS "NX_STL_DEBUG: libc++ hardening mode DEBUG "
                           "(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)")
            add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG)
        else()
            # libc++ before 18 called it _LIBCPP_ENABLE_ASSERTIONS. Only use it if it demonstrably maps onto the
            # hardening machinery (non-NONE mode); otherwise say so loudly.
            set(CMAKE_REQUIRED_DEFINITIONS "-D_LIBCPP_ENABLE_ASSERTIONS=1")
            check_cxx_source_compiles("
#include <vector>
#if !defined(_LIBCPP_HARDENING_MODE) || _LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_NONE
#  error libc++ ignored _LIBCPP_ENABLE_ASSERTIONS
#endif
int main() { std::vector<int> v; return static_cast<int>(v.size()); }
" NX_STL_LIBCXX_LEGACY_ASSERTIONS)
            if(NX_STL_LIBCXX_LEGACY_ASSERTIONS)
                message(STATUS "NX_STL_DEBUG: libc++ assertions via _LIBCPP_ENABLE_ASSERTIONS=1 (legacy libc++)")
                add_compile_definitions(_LIBCPP_ENABLE_ASSERTIONS=1)
            else()
                message(WARNING
                    "NX_STL_DEBUG is ON but this libc++ supports neither _LIBCPP_HARDENING_MODE_DEBUG nor "
                    "_LIBCPP_ENABLE_ASSERTIONS: checked STL containers are NOT enabled. Update the toolchain or drop "
                    "NX_STL_DEBUG (UBSan still works). See docs/SANITIZERS.md.")
            endif()
        endif()
        set(CMAKE_REQUIRED_DEFINITIONS "${_nx_saved_required_definitions}")
    else()
        message(WARNING
            "NX_STL_DEBUG is ON but the standard library in use is neither libstdc++ nor libc++ (MSVC STL?): checked "
            "STL containers are NOT enabled. On MSVC use a Debug build (/D_ITERATOR_DEBUG_LEVEL=2); otherwise update "
            "the toolchain. See docs/SANITIZERS.md.")
    endif()
endif()

# ---- NX_SANITIZE: configure-time link probe, then flags -------------------------------------------------------

if(NX_SANITIZE_LIST)
    if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "NX_SANITIZE is not supported with MSVC: it has no -fsanitize=undefined. Build with clang-cl or clang "
            "(see docs/SANITIZERS.md), or configure without NX_SANITIZE.")
    endif()

    # The sanitizer runtime is linked by the compiler driver itself; -fno-sanitize-recover=all is codegen-only but
    # harmless on the link line, and having one flag list keeps the probe identical to what targets will get.
    string(REPLACE ";" "," _nx_sanitize_flag_value "${NX_SANITIZE_LIST}")
    set(_nx_sanitize_compile_flags "-fsanitize=${_nx_sanitize_flag_value}")
    if(NX_SANITIZE_RECOVER)
        list(APPEND _nx_sanitize_compile_flags "-fsanitize-recover=all")
    else()
        list(APPEND _nx_sanitize_compile_flags "-fno-sanitize-recover=all")
    endif()
    list(APPEND _nx_sanitize_compile_flags "-fno-omit-frame-pointer")
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND _nx_sanitize_compile_flags "-g")
    endif()
    set(_nx_sanitize_link_flags "-fsanitize=${_nx_sanitize_flag_value}")

    # A real link, not a file-existence check: the same flags and the same compiler the targets will use.
    # Cache name includes compiler id/version and the requested sanitizers, so reconfiguring with a changed
    # compiler or NX_SANITIZE re-probes instead of trusting a stale result.
    set(_nx_sanitize_probe_tag "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${_nx_sanitize_flag_value}")
    string(MAKE_C_IDENTIFIER "${_nx_sanitize_probe_tag}" _nx_sanitize_probe_var)
    string(TOUPPER "${_nx_sanitize_probe_var}" _nx_sanitize_probe_var)
    set(_nx_sanitize_probe_var "NX_SANITIZE_LINK_${_nx_sanitize_probe_var}")

    set(_nx_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(_nx_saved_required_link_options "${CMAKE_REQUIRED_LINK_OPTIONS}")
    set(_nx_saved_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
    unset(CMAKE_REQUIRED_DEFINITIONS)
    string(REPLACE ";" " " _nx_probe_flag_string "${_nx_sanitize_compile_flags}")
    set(CMAKE_REQUIRED_FLAGS "${_nx_probe_flag_string}")
    # NX_LLVM_ROOT: if a hand-installed LLVM is pointed at, also let the probe see its compiler-rt library
    # directories (-L), in case the selected driver does not add them itself.
    if(NX_LLVM_ROOT)
        file(GLOB _nx_llvm_runtime_dirs "${NX_LLVM_ROOT}/lib/clang/*/lib/*")
        set(_nx_llvm_link_options)
        foreach(_nx_dir IN LISTS _nx_llvm_runtime_dirs)
            list(APPEND _nx_llvm_link_options "-L${_nx_dir}")
        endforeach()
        set(CMAKE_REQUIRED_LINK_OPTIONS ${_nx_llvm_link_options})
    endif()

    check_cxx_source_compiles("int main() { return 0; }" ${_nx_sanitize_probe_var})

    if(NOT ${_nx_sanitize_probe_var})
        _nx_sanitizer_runtime_dir(_nx_rt_dir "${CMAKE_CXX_COMPILER}")
        _nx_sanitizer_runtime_libs(_nx_rt_libs "${_nx_rt_dir}")
        _nx_sanitizer_link_log(_nx_link_log ${_nx_sanitize_compile_flags})

        if(_nx_rt_dir)
            set(_nx_rt_dir_line "  clang --print-runtime-dir: ${_nx_rt_dir}")
            if(_nx_rt_libs)
                set(_nx_rt_libs_line "  libclang_rt.* runtimes there: ${_nx_rt_libs}")
            else()
                set(_nx_rt_libs_line "  libclang_rt.* runtimes there: none for ${_nx_sanitize_flag_value} "
                                     "(that is the problem)")
            endif()
        else()
            set(_nx_rt_dir_line "  clang --print-runtime-dir: not supported by this compiler (or it failed)")
            set(_nx_rt_libs_line "  compiler-rt sanitizer runtime: could not be located")
        endif()
        if(NX_LLVM_ROOT)
            set(_nx_llvm_line "  NX_LLVM_ROOT: ${NX_LLVM_ROOT}")
        else()
            set(_nx_llvm_line "  NX_LLVM_ROOT: (not set)")
        endif()

        message(FATAL_ERROR
"NX_SANITIZE=${_nx_sanitize_flag_value} does not link with the selected compiler: the sanitizer runtime
(compiler-rt) is missing or not visible.

  compiler:  ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})
  requested: ${_nx_sanitize_flag_value}
  probe:     ${_nx_probe_flag_string}
${_nx_rt_dir_line}
${_nx_rt_libs_line}
${_nx_llvm_line}
  linker output (tail):
  ${_nx_link_log}

How to fix it:
  * macOS: the Xcode command line tools ship the runtime. Check \`xcode-select -p\`, then
    \`\"$(xcrun --find clang++)\" --print-runtime-dir\` and that it contains libclang_rt.ubsan_osx_dynamic.dylib.
  * Linux / Steam Deck: install clang together with compiler-rt (e.g. Debian/Ubuntu
    \`sudo apt install clang libclang-rt-<version>-dev\`, Arch \`sudo pacman -S clang compiler-rt\`), or unpack the
    official LLVM release tarball and configure with
        -DCMAKE_C_COMPILER=<llvm>/bin/clang -DCMAKE_CXX_COMPILER=<llvm>/bin/clang++
    (or set NX_LLVM_ROOT=<llvm> and use scripts/build_linux.sh). Verify with
    \`clang --print-runtime-dir\` and \`ls \"$(clang --print-runtime-dir)\"/libclang_rt.ubsan*\`.
  * Windows: use clang-cl from an LLVM install with compiler-rt; MSVC has no UBSan.
  * Or configure without sanitizers: -DNX_SANITIZE= (empty). Default Release builds are unaffected.
  See docs/SANITIZERS.md, \"Getting a compiler-rt-enabled LLVM\".")

    endif()

    set(CMAKE_REQUIRED_FLAGS "${_nx_saved_required_flags}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${_nx_saved_required_link_options}")
    set(CMAKE_REQUIRED_DEFINITIONS "${_nx_saved_required_definitions}")

    # Report where the runtime comes from, for the configure log and for docs/evidence.
    _nx_sanitizer_runtime_dir(_nx_rt_dir "${CMAKE_CXX_COMPILER}")
    _nx_sanitizer_runtime_libs(_nx_rt_libs "${_nx_rt_dir}")
    if(_nx_rt_dir AND _nx_rt_libs)
        message(STATUS "NX_SANITIZE=${_nx_sanitize_flag_value}: runtime links; compiler-rt at ${_nx_rt_dir} "
                       "(${_nx_rt_libs})")
    else()
        message(STATUS "NX_SANITIZE=${_nx_sanitize_flag_value}: runtime links (compiler-rt location not reported "
                       "by ${CMAKE_CXX_COMPILER})")
    endif()

    add_compile_options(${_nx_sanitize_compile_flags})
    add_link_options(${_nx_sanitize_link_flags})
    set(NX_SANITIZE_ACTIVE TRUE)

    # Flags exported to scripts/ubsan_smoke.sh (one "FLAG=" line per compile/link flag, no shell quoting needed).
    set(_nx_sanitizer_flags_file "${CMAKE_BINARY_DIR}/nx-sanitizer-flags.txt")
    file(WRITE "${_nx_sanitizer_flags_file}"
        "# Generated by cmake/NXSanitizers.cmake for scripts/ubsan_smoke.sh. Do not edit.\n"
        "COMPILER=${CMAKE_CXX_COMPILER}\n"
        "RECOVER=${NX_SANITIZE_RECOVER}\n"
        "SANITIZERS=${_nx_sanitize_flag_value}\n")
    foreach(_nx_flag IN LISTS _nx_sanitize_compile_flags)
        file(APPEND "${_nx_sanitizer_flags_file}" "FLAG=${_nx_flag}\n")
    endforeach()
endif()

if(NX_SANITIZE_ACTIVE AND NX_STL_DEBUG)
    message(STATUS "Checked build: -fsanitize=${_nx_sanitize_flag_value} + checked STL + "
                   "nx::fatal()/static_assert checks (docs/SANITIZERS.md)")
endif()
