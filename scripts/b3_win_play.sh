#!/usr/bin/env bash
# Launch Bayonetta 3 on the Windows host with the settings from private/b3.conf.
#
#   scripts/b3_win_play.sh [runtime arguments...]
#
# This is the Windows counterpart of scripts/b3_play.sh: same settings file, same `key=value` -> `NX_<KEY>`
# rule, same "false/off/no/empty unset, and 0 too except for the numeric pad/fsr settings". The difference is
# what happens after the settings are applied: b3_play.sh execs scripts/run_linux.sh, which builds a symlinked
# data root and loops the program-0/program-1 handoff; this one links the data roots with NTFS junctions
# (scripts/lib/b3_common.sh: b3_win_link_data_root, b3_win_link_prog1_root) and runs the same handoff loop
# itself. The launcher (tools/b3_launcher.py) calls this file on Windows.
#   PROG1_EXE, PROG1_DATA_ROOT  program 1 (default private/build-win-prog1, private/run-win-prog1)
#
# Environment:
#   EXE         game binary   (default: b3_default_exe -> private/build-win/bayonetta3.exe)
#   DATA_ROOT   data root     (default: b3_default_data_root -> private/run-win)
#   B3_CONFIG   settings file (default: <repo>/private/b3.conf)
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf '%s\n' \
        'Usage: scripts/b3_win_play.sh [runtime arguments...]' \
        'Reads private/b3.conf, links the data root, then runs the Windows build.' \
        'EXE and DATA_ROOT override the defaults (private/build-win, private/run-win).' \
        "Log: NX_LOG if set, else <repo>/private/b3-win.log"
    exit 0
fi

CONF=${B3_CONFIG:-$ROOT/private/b3.conf}
TEMPLATE=$ROOT/config/b3.conf.example
if [[ ! -f $CONF && -f $TEMPLATE ]]; then
    mkdir -p "$(dirname "$CONF")"
    cp -- "$TEMPLATE" "$CONF"
    printf 'b3: created %s from the template; edit it and relaunch\n' "$CONF"
fi

applied=0
if [[ -f $CONF ]]; then
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%%#*}                       # comments
        line=${line#"${line%%[![:space:]]*}"}   # leading space
        line=${line%"${line##*[![:space:]]}"}   # trailing space
        [[ $line == *=* ]] || continue
        key=${line%%=*}; value=${line#*=}
        key=${key%"${key##*[![:space:]]}"}
        value=${value#"${value%%[![:space:]]*}"}
        [[ $key =~ ^[A-Za-z0-9_]+$ ]] || { printf 'b3: ignoring bad key in %s: %s\n' "$CONF" "$key" >&2; continue; }
        if [[ $key == NX_* ]]; then
            name=$key
        else
            name=NX_${key^^}
        fi
        case ${value,,} in
            false|off|no|"") continue ;;
            0)
                # These numeric settings accept zero as a value, rather than treating it as an off switch.
                case ${name^^} in
                    NX_PAD_DEADZONE|NX_PAD_TRIGGER|NX_PAD_INDEX|NX_FSR_SHARP|NX_MOUSE_CAMERA) ;;
                    *) continue ;;
                esac
                ;;
        esac
        export "$name=$value"
        applied=$((applied + 1))
    done <"$CONF"
fi

EXE=${EXE:-$(b3_default_exe)}
DATA_ROOT=${DATA_ROOT:-$(b3_default_data_root)}
[[ -n ${NX_LOG:-} ]] || export NX_LOG=$ROOT/private/b3-win.log
[[ -n ${NX_DATA_ROOT:-} ]] || export NX_DATA_ROOT=$DATA_ROOT
[[ -n ${NX_INPUT_LIVE:-} ]] || export NX_INPUT_LIVE="$(b3_win_path "$DATA_ROOT")\\input_live.txt"
[[ -f $CONF ]] && export NX_SETTINGS_FILE="$(b3_win_path "$CONF")"  # the in-game settings panel (F1) saves here

[[ -f $EXE ]] || { printf 'error: no executable %s; build it first (docs/BUILD.md)\n' "$EXE" >&2; exit 1; }

# The runtime opens data_root()/analysis, data_root()/spirv and data_root()/extract/romfs; the inputs are
# read-only and live outside the repo, so they are linked rather than copied.
b3_win_link_data_root "$DATA_ROOT" || exit 1

printf 'b3: %d setting(s) from %s; data %s; log %s\n' "$applied" "$CONF" "$DATA_ROOT" "$NX_LOG"

# The program switch, as in scripts/run_linux.sh: the hub's picture book calls ExecuteProgram(1), the runtime
# writes relaunch.txt and exits with 86, and this loop starts program 1 (the Bayonetta Origins prologue), handing
# it what the old program pushed to the user channel; program 1 ends with ExecuteProgram(0) the same way.
PROG1_EXE=${PROG1_EXE:-$ROOT/private/build-win-prog1/bayonetta3prog1.exe}
PROG1_DATA_ROOT=${PROG1_DATA_ROOT:-$ROOT/private/run-win-prog1}
program=0
attempts=0
while :; do
    if [[ $program == 0 ]]; then
        exe=$EXE root=$DATA_ROOT
        run_env=()
    else
        exe=$PROG1_EXE root=$PROG1_DATA_ROOT
        b3_win_link_prog1_root "$PROG1_DATA_ROOT" "$DATA_ROOT" || exit 1
        run_env=("NX_DATA_ROOT=$(b3_win_path "$root")" "NX_INPUT_LIVE=$(b3_win_path "$root")\\input_live.txt"
                 "NX_LOG=$(b3_win_path "${NX_LOG%.log}-prog1.log")")
    fi
    [[ -f $exe ]] || { printf 'error: program %d binary missing: %s\n' "$program" "$exe" >&2; exit 1; }
    # The game's Win32 calls cannot open /c/... paths.
    env "${run_env[@]}" "$(b3_win_path "$exe")" "$@"
    rc=$?
    if [[ $rc == 86 && $attempts -lt 8 ]]; then
        attempts=$((attempts + 1))
        kind=0; value=0
        [[ -f $root/relaunch.txt ]] && read -r kind value <"$root/relaunch.txt"
        value=${value%$'\r'}  # a file written in Windows text mode ends its line with CRLF
        next=0
        [[ $kind == 0 && $value == 1 ]] && next=1
        nextroot=$DATA_ROOT
        if [[ $next == 1 ]]; then
            nextroot=$PROG1_DATA_ROOT
            b3_win_link_prog1_root "$PROG1_DATA_ROOT" "$DATA_ROOT" || exit 1
        fi
        [[ -f $root/user_channel.bin ]] && cp -f "$root/user_channel.bin" "$nextroot/launch_parameter.txt"
        rm -f "$root/relaunch.txt"
        printf 'b3: program %d asked for program %d (kind %s, value %s); starting it (%d)\n' \
            "$program" "$next" "$kind" "$value" "$attempts"
        program=$next
        continue
    fi
    exit $rc
done
