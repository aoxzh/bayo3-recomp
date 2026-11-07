#!/usr/bin/env bash
# Launch Bayonetta 3 with persistent settings (one place for the NX_* switches).
#
#   scripts/b3_play.sh [runtime arguments...]
#
# Settings file, first found: $B3_CONFIG, <repo>/private/b3.conf (created from config/b3.conf.example
# on first launch). Each `key=value` line becomes NX_<KEY> in the environment before the runtime
# starts. false/off/no/empty leave a switch unset; 0 does too, except for the numeric settings
# pad_deadzone, pad_trigger, pad_index and fsr_sharp, which accept 0 as a value. A line that already
# starts with NX_ is exported verbatim.
#
# The game binary defaults to private/build-deck/bayonetta3 (Release); override with BUILD_DIR or EXE.
# The data root and the analysis/extract/spirv links are set up by scripts/run_linux.sh, which this
# script execs. The log defaults to <repo>/private/b3.log and is printed at the end of this script.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

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
[[ -n ${NX_LOG:-} ]] || export NX_LOG=$ROOT/private/b3.log
printf 'b3: %d setting(s) from %s; log %s\n' "$applied" "$CONF" "$NX_LOG"

BUILD_DIR=${BUILD_DIR:-$ROOT/private/build-deck}
export BUILD_DIR
# exec keeps this PID, which is what the clock watcher follows until the game ends.
if [[ -n ${NX_GPU_CLOCK:-} ]]; then
    . "$ROOT/scripts/lib/b3_gpu_clock.sh"
    b3_gpu_clock_hold "$NX_GPU_CLOCK" $$
fi
exec "$ROOT/scripts/run_linux.sh" "$@"
