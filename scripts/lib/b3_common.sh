#!/usr/bin/env bash
# Shared run contract for every Bayonetta 3 entry point.
#
# WHY THIS FILE EXISTS
#   A fix verified on one host does not automatically hold on the other. Three concrete ways it broke
#   (docs/CROSS-PLATFORM.md has the full case studies):
#     * a repair that only ran when a diagnostic switch was set  (ev0005 attach, commit 0d004e4),
#     * per-platform entry scripts drifting apart                  (--game-log only on one side),
#     * acceptance evidence living only on the machine that ran it (logs/ is gitignored).
#   So the contract lives in exactly one place:
#     - one definition of the autodrive invocation (b3_driver_args): both hosts pass the same flags,
#     - one definition of the run environment (b3_export_base_env) and of the clean-environment rule
#       (b3_strip_diagnostic_env): "default" means "no diagnostic NX_* switch set",
#     - one lock/root/summary convention, so a script cannot quietly do its own thing,
#     - B3_DRY_RUN=1: assemble every command and print it instead of launching a game, so the
#       contract is testable without a GPU (tests/test_cross_platform_harness.py uses this).
#
#   Entry points MUST source this file and MUST NOT hand-build `tools/b3_autodrive.py` arguments.
#   scripts/b3_preflight.sh and tests/test_cross_platform_harness.py enforce both.
#
# Bash 3.2 compatible: macOS ships /bin/bash 3.2, so no ${x,,}, no associative arrays, no mapfile.

B3_COMMON_VERSION=1

# --- identity ------------------------------------------------------------------------------------
b3_repo_root() {
    # The repository root, independent of where the caller was started from.
    if [ -n "${B3_ROOT:-}" ]; then printf '%s' "$B3_ROOT"; return; fi
    local here
    here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
    printf '%s' "$here"
}

b3_platform() {
    # b3-linux | b3-macos | b3-win | b3-android. B3_PLATFORM overrides (tests use it to exercise both paths).
    if [ -n "${B3_PLATFORM:-}" ]; then printf '%s' "$B3_PLATFORM"; return; fi
    case "$(uname -s)" in
        Darwin) printf 'b3-macos' ;;
        Linux)  printf 'b3-linux' ;;
        # Git Bash, MSYS2 and Cygwin all report MINGW64_NT-*, MSYS_NT-* or CYGWIN_NT-*.
        MINGW*|MSYS*|CYGWIN*) printf 'b3-win' ;;
        *)      printf 'b3-unknown' ;;
    esac
}

b3_on_windows() {
    case "$(b3_platform)" in b3-win) return 0 ;; *) return 1 ;; esac
}

b3_win_path() {
    # A POSIX path as a Windows one: the Win32 calls inside the game (CreateFile, LoadLibrary) do not
    # accept /c/... or a bare / at all. cygpath ships with Git Bash, MSYS2 and Cygwin.
    local path=${1:?path}
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w -- "$path"
    else
        printf '%s' "$path"
    fi
}

b3_win_game_pids() {
    # The PIDs running the game's image. There is no /proc on Windows, so this goes through tasklist and
    # matches the image name exactly: tasklist's default table truncates the name, so /FO CSV is used.
    local exe=${1:-bayonetta3.exe}
    command -v tasklist >/dev/null 2>&1 || return 0
    tasklist //FO CSV //NH //FI "IMAGENAME eq $exe" 2>/dev/null |
        awk -F'","' -v want="$exe" '{ gsub(/"/, "", $1); if (tolower($1) == tolower(want)) print $2 }'
}

b3_win_kill_game() {
    local exe=${1:-bayonetta3.exe}
    command -v taskkill >/dev/null 2>&1 || return 0
    taskkill //F //IM "$exe" >/dev/null 2>&1 || true
}

b3_win_junction() {
    # b3_win_junction <link> <target>: create an NTFS junction when the link is absent. A junction
    # (mklink /J) needs neither developer mode nor elevation, which a symlink does on Windows.
    local link=${1:?link} target=${2:?target}
    [ -e "$link" ] && return 0
    [ -d "$target" ] || { printf 'error: junction target missing: %s\n' "$target" >&2; return 1; }
    cmd //c mklink //J "$(b3_win_path "$link")" "$(b3_win_path "$target")" >/dev/null ||
        { printf 'error: could not link %s -> %s\n' "$link" "$target" >&2; return 1; }
}

b3_win_link_data_root() {
    # The data root the runtime reads: analysis/, spirv/ and extract/romfs/, each a junction to the
    # read-only inputs. An existing link is left alone; a real directory in the way is that directory's
    # problem, not this script's (silently replacing it is how a stale analysis file survives a recompile).
    local root=${1:?data root}
    local repo; repo=$(b3_repo_root)
    local generated=${B3_WIN_GENERATED:-$repo/private/bayonetta3}
    local spirv=${B3_SPIRV_DIR:-$repo/private/bayonetta3-shaders-tess-hmflower-v1/spirv}
    local game=${B3_WIN_GAME_ROOT:-${B3_GAME_ROOT:-$repo/private/game}}
    mkdir -p "$root/extract"
    b3_win_junction "$root/analysis"      "$generated/analysis" || return 1
    b3_win_junction "$root/spirv"         "$spirv"              || return 1
    b3_win_junction "$root/extract/romfs" "$game/RomFS"         || return 1
    return 0
}

b3_win_link_prog1_root() {
    # b3_win_link_prog1_root <program 1 root> <program 0 root>: program 1 (the picture book's Bayonetta Origins
    # prologue) reads its own analysis, RomFS and shaders, and the main program's user/: both programs of one
    # application share the save, as on the console.
    local root=${1:?program 1 data root} main_root=${2:?program 0 data root}
    local repo; repo=$(b3_repo_root)
    mkdir -p "$root/extract" "$main_root/user"
    b3_win_junction "$root/analysis"      "${B3_PROG1_GENERATED:-$repo/private/prog1-out}/analysis" || return 1
    b3_win_junction "$root/spirv"         "${B3_PROG1_SPIRV_DIR:-$repo/private/prog1-shaders/spirv}" || return 1
    b3_win_junction "$root/extract/romfs" "${B3_PROG1_ROMFS:-$repo/private/prog1/romfs}"         || return 1
    b3_win_junction "$root/user"          "$main_root/user"                                      || return 1
    return 0
}

b3_ra2_root() {
    # A configured external data root remains supported; the default is repo-local.
    local repo; repo=$(b3_repo_root)
    printf '%s' "${B3_RA2_ROOT:-$repo/private}"
}

b3_default_exe() {
    local root; root=$(b3_repo_root)
    case "$(b3_platform)" in
        b3-macos)
            # Prefer the repo-local build; configured B3_MAC_BUILD_DIR or B3_RA2_ROOT may select another.
            local local_build="$root/private/build-mac/bayonetta3"
            if [ -n "${B3_MAC_BUILD_DIR:-}" ]; then
                printf '%s' "$B3_MAC_BUILD_DIR/bayonetta3"
            elif [ -d "$root/private/build-mac" ]; then
                printf '%s' "$local_build"
            else
                printf '%s' "$(b3_ra2_root)/build-mac/bayonetta3"
            fi ;;

        b3-linux) printf '%s' "$root/private/build-deck/bayonetta3" ;;
        b3-win)   printf '%s' "$root/private/build-win/bayonetta3.exe" ;;
        *)        printf '%s' "$root/private/build-deck/bayonetta3" ;;
    esac
}

# The PIDs actually executing `exe`, resolved through /proc/<pid>/exe. `pgrep -f "$EXE"` also matches
# any process whose command line merely mentions the path (a concurrent `md5sum private/bin/b3-rc8`
# aborted a run on 2026-10-27 with "a game process from another run is alive"), and the path in
# /proc/<pid>/exe gains " (deleted)" when a rebuild replaces the binary under a running game.
b3_game_pids() {
    local want real pid
    if b3_on_windows; then
        b3_win_game_pids "$(basename -- "${1:-bayonetta3.exe}")"
        return
    fi
    want=$(readlink -f -- "$1" 2>/dev/null || printf '%s' "$1")
    for pid in /proc/[0-9]*; do
        pid=${pid#/proc/}
        real=$(readlink "/proc/$pid/exe" 2>/dev/null) || continue
        real=${real% (deleted)}
        [ "$real" = "$want" ] && printf '%s\n' "$pid"
    done
}

b3_default_data_root() {
    local root; root=$(b3_repo_root)
    case "$(b3_platform)" in
        b3-macos) printf '%s' "${B3_MAC_DATA_ROOT:-$root/private/B3-data/run}" ;;
        b3-win)   printf '%s' "${B3_WIN_DATA_ROOT:-$root/private/run-win}" ;;
        *)        printf '%s' "$root/private/run-vt" ;;
    esac
}

b3_default_logs() {
    local root; root=$(b3_repo_root)
    case "$(b3_platform)" in
        b3-macos) printf '%s' "$root/logs/mac" ;;
        b3-win)   printf '%s' "$root/logs/win" ;;
        *)        printf '%s' "$root/logs/ch1" ;;
    esac
}

b3_git_commit() {
    local root; root=$(b3_repo_root)
    git -C "$root" rev-parse HEAD 2>/dev/null || printf 'unknown'
}

b3_python() {
    # A python3 that actually runs a script. On Windows `python3` in PATH is usually the Microsoft
    # Store *stub* (WindowsApps/python3.exe): it prints "Python was not found" and exits 49 instead of
    # running anything, so `command -v python3` succeeding proves nothing. Probe by running it.
    # Cached in B3_PYTHON_CACHE, because every candidate costs a process start. B3_PYTHON overrides.
    if [ -n "${B3_PYTHON:-}" ]; then printf '%s' "$B3_PYTHON"; return 0; fi
    if [ -n "${B3_PYTHON_CACHE:-}" ]; then printf '%s' "$B3_PYTHON_CACHE"; return 0; fi
    local candidate
    for candidate in python3 python; do
        command -v "$candidate" >/dev/null 2>&1 || continue
        "$candidate" -c 'import sys' >/dev/null 2>&1 || continue
        B3_PYTHON_CACHE=$candidate
        printf '%s' "$candidate"
        return 0
    done
    printf 'python3'
    return 1
}

# --- dry run -------------------------------------------------------------------------------------
# B3_DRY_RUN=1: print the assembled plan instead of starting a game. Every line carries the
# B3_DRY_RUN: prefix so a test can read the plan without parsing prose.
b3_dry_run() { [ "${B3_DRY_RUN:-0}" = "1" ]; }

b3_dry_run_report() {
    # stdin: one "key: value" line per assembled command; prints them with the marker prefix.
    local line
    while IFS= read -r line; do
        [ -n "$line" ] && printf 'B3_DRY_RUN: %s\n' "$line"
    done
}

# --- machine lock --------------------------------------------------------------------------------
# One GPU per host, so builds and runs serialise. flock where it exists (Linux), mkdir otherwise
# (macOS has no flock). Both degrade to the same contract: acquire, hold, release.
B3_LOCK_DIR=
B3_LOCK_FD=

b3_lock_acquire() {
    # b3_lock_acquire <name> [timeout seconds]
    local name=${1:?lock name} timeout=${2:-7200}
    local root; root=$(b3_repo_root)
    mkdir -p "$root/private"
    # The lock *kind* is part of the contract, not an accident of PATH: the macOS build script takes
    # private/.b3-mac-lock as a directory, so a host that later grows an flock(1) must still use mkdir
    # there or the two scripts stop excluding each other. B3_LOCK_MODE=flock|mkdir overrides.
    local mode=${B3_LOCK_MODE:-auto}
    if [ "$mode" = "auto" ]; then
        case "$(b3_platform)" in
            b3-macos) mode=mkdir ;;
            *)        mode=flock ;;
        esac
    fi
    if [ "$mode" = "flock" ] && command -v flock >/dev/null 2>&1; then
        B3_LOCK_FD=9
        eval "exec $B3_LOCK_FD>\"\$root/private/.$name-lock\""
        if ! eval "flock -w $timeout $B3_LOCK_FD"; then
            printf 'error: another run holds the %s lock\n' "$name" >&2
            return 1
        fi
        B3_LOCK_DIR="flock:$B3_LOCK_FD"
        return 0
    fi
    B3_LOCK_DIR=$root/private/.$name-lock
    local waited=0
    while ! mkdir "$B3_LOCK_DIR" 2>/dev/null; do
        if [ "$waited" -ge "$timeout" ]; then
            printf 'error: the %s lock is stuck: %s\n' "$name" "$B3_LOCK_DIR" >&2
            return 1
        fi
        sleep 2
        waited=$((waited + 2))
    done
    return 0
}

b3_lock_release() {
    case "${B3_LOCK_DIR:-}" in
        flock:*) eval "exec ${B3_LOCK_DIR#flock:}>&-" 2>/dev/null || true ;;
        "") : ;;
        *) rmdir "$B3_LOCK_DIR" 2>/dev/null || true ;;
    esac
    B3_LOCK_DIR= B3_LOCK_FD=
}

# --- default environment vs diagnostic environment ------------------------------------------------
# The rule: a *repair* must be on with no switch set. A run that claims a fix is verified therefore
# runs with every diagnostic switch cleared. b3_strip_diagnostic_env is that operation, and it
# reports exactly what it removed so the acceptance record can show it was a default-environment run.
B3_CLEAN_ENV_KEEP="NX_DATA_ROOT NX_LOG NX_TMPDIR"

b3_strip_diagnostic_env() {
    # Unset every NX_* except the keep list. Prints "stripped: NX_FOO=1" per variable.
    local keep=" $B3_CLEAN_ENV_KEEP " var val
    for var in $(compgen -v 2>/dev/null | grep '^NX_' || true); do
        case "$keep" in *" $var "*) continue ;; esac
        eval "val=\${$var:-}"
        printf 'stripped: %s=%s\n' "$var" "$val"
        unset "$var"
    done
}

b3_export_base_env() {
    # The environment every headless measurement run uses. Callers lift switches from here only.
    local data_root=${1:?data root}
    local root; root=$(b3_repo_root)
    local live
    if b3_on_windows; then
        # The game is a native Win32 program: a /c/... path is not a path it can open.
        live=$(b3_win_path "$data_root")"\\input_live.txt"
    else
        live="$data_root/input_live.txt"
    fi
    export NX_DATA_ROOT="$data_root"
    export TMPDIR=${TMPDIR_OVERRIDE:-$root/private/tmp}
    export NX_HEADLESS=1
    export NX_MUTE=1
    export NX_INPUT_LIVE="$live"
    export NX_PRESENT_TIMING=1
    mkdir -p "$TMPDIR"
}

b3_clear_run_markers() {
    # A leftover marker from an earlier run would start a trace/dump at boot.
    local data_root=${1:?data root}
    rm -f "$data_root/dump_frames" "$data_root/trace_present" "$data_root/autodrive_run"
    : > "$data_root/input_live.txt"
}

# --- the autodrive contract (single source of truth) ---------------------------------------------
# Every flag here is part of the contract: if one host passes --game-log and the other does not, the
# driver's movie-aware branch silently dies on that host (that is exactly what happened before
# 0d004e4). Entry points call b3_driver_args; they never spell the flags out.
b3_driver_contract_flags() {
    printf '%s\n' --game-log --log --every --timeout --a-only-until
}

b3_driver_args() {
    # b3_driver_args --root DIR --game-log FILE --drive-log FILE --timeout SECONDS
    #                [--every SEC] [--a-only-until SEC] [--take-yes] [--movie-frames SEC]
    #                [--movie-skip-after SEC]
    local root= game_log= drive_log= timeout= every=1.5 a_only=0 take_yes=0
    local movie_frames= movie_skip=
    while [ $# -gt 0 ]; do
        case "$1" in
            --root) root=$2; shift 2 ;;
            --game-log) game_log=$2; shift 2 ;;
            --drive-log) drive_log=$2; shift 2 ;;
            --timeout) timeout=$2; shift 2 ;;
            --every) every=$2; shift 2 ;;
            --a-only-until) a_only=$2; shift 2 ;;
            --take-yes) take_yes=1; shift ;;
            --movie-frames) movie_frames=$2; shift 2 ;;
            --movie-skip-after) movie_skip=$2; shift 2 ;;
            *) printf 'b3_driver_args: unknown argument %s\n' "$1" >&2; return 2 ;;
        esac
    done
    [ -n "$root" ] && [ -n "$timeout" ] || {
        printf 'b3_driver_args: --root and --timeout are required\n' >&2; return 2; }
    local root_repo; root_repo=$(b3_repo_root)
    printf '%s\n' "$(b3_python)" "$root_repo/tools/b3_autodrive.py" "$root"
    printf '%s\n' --every "$every" --timeout "$timeout" --a-only-until "$a_only"
    # --game-log is unconditional: the movie-aware branch of the driver keys on it.
    printf '%s\n' --game-log "$game_log"
    [ -n "$drive_log" ] && printf '%s\n' --log "$drive_log"
    [ "$take_yes" = "1" ] && printf '%s\n' --take-yes
    [ -n "$movie_frames" ] && printf '%s\n' --movie-frames "$movie_frames"
    [ -n "$movie_skip" ] && printf '%s\n' --movie-skip-after "$movie_skip"
    return 0
}

b3_start_autodrive() {
    # Background the driver; sets B3_DRIVE_PID. Args are b3_driver_args' arguments.
    local argv=()
    local line
    while IFS= read -r line; do argv+=("$line"); done < <(b3_driver_args "$@")
    # Keep diagnostics visible by default; callers may preserve them in a dedicated run log.
    "${argv[@]}" >/dev/null 2>"${B3_DRIVE_STDERR:-/dev/stderr}" &
    B3_DRIVE_PID=$!
    printf '%s' "$B3_DRIVE_PID"
}

# --- summary (one format for both hosts) ---------------------------------------------------------
b3_write_summary() {
    # b3_write_summary <summary file> <name> <exe> <data root> <wall seconds> <game log> [extra env text]
    local out=${1:?summary file} name=$2 exe=$3 data_root=$4 wall=$5 game_log=$6 extra=${7:-}
    local fatal crash logs_dir
    logs_dir=$(dirname -- "$out")
    mkdir -p "$logs_dir"
    fatal=$(grep -c FATAL "$game_log" 2>/dev/null || true)
    crash=$(grep -c 'CRASH' "$game_log" 2>/dev/null || true)
    {
        printf 'name: %s\n' "$name"
        printf 'exe: %s\n' "$exe"
        printf 'data root: %s\n' "$data_root"
        printf 'commit: %s\n' "$(b3_git_commit)"
        printf 'platform: %s\n' "$(b3_platform)"
        printf 'env: %s\n' "$extra"
        printf 'wall: %s s\n' "$wall"
        printf 'fatal lines: %s\n' "${fatal:-0}"
        printf 'crash lines: %s\n' "${crash:-0}"
        grep 'per frame over 60' "$game_log" 2>/dev/null | tail -3
        grep 'movie:' "$game_log" 2>/dev/null | tail -5
        grep 'movie-vt: attached movie system' "$game_log" 2>/dev/null | tail -3
    } > "$out"
    cat "$out"
}
