# Hold the Steam Deck GPU at a fixed clock while the game runs, and give the clock back when it ends.
# Sourced by scripts/b3_play.sh and the benchmark scripts.
#
#   b3_gpu_clock_hold <mhz> <pid>
#
# Why: under `auto` the 15 W APU firmware favours the CPU cores. This game keeps the CPU busy, so the GPU
# sat at 200 MHz with work queued. The same fight took 12-55 ms of GPU time a frame under `auto` and a
# steady 8 ms at 1600 MHz, with package power unchanged at the 15 W cap (docs/PERF-DECK-GPU-CLOCK-2026-10-31.md).
#
# This is what Steam's "Manual GPU Clock" slider does, through the same SteamOS helper. The power cap is
# not touched. A level that is already `manual` belongs to the user or to Steam: it is left alone.
#
# The clock is restored by a detached watcher rather than a trap, so a crash or SIGKILL of the game still
# gives it back. Only a power loss skips the watcher, and a reboot resets the setting anyway.
B3_GPU_CARD_DIR=${B3_GPU_CARD_DIR:-/sys/class/drm/card0/device}
B3_GPU_PRIV_WRITE=${B3_GPU_PRIV_WRITE:-/usr/bin/steamos-polkit-helpers/steamos-priv-write}

b3_gpu_clock_write() {  # <attribute> <value>
    "$B3_GPU_PRIV_WRITE" "$B3_GPU_CARD_DIR/$1" "$2" >/dev/null 2>&1
}

b3_gpu_clock_release() {
    b3_gpu_clock_write pp_od_clk_voltage r
    b3_gpu_clock_write pp_od_clk_voltage c
    b3_gpu_clock_write power_dpm_force_performance_level auto
}

b3_gpu_clock_hold() {
    local mhz=${1:?clock in MHz} pid=${2:?process to watch} level
    [[ $mhz =~ ^[0-9]+$ ]] && ((mhz >= 200 && mhz <= 1600)) || {
        printf 'b3: gpu_clock must be 200..1600, got %s; clock left alone\n' "$mhz" >&2
        return 0
    }
    [[ -x $B3_GPU_PRIV_WRITE && -r $B3_GPU_CARD_DIR/power_dpm_force_performance_level ]] || return 0
    level=$(<"$B3_GPU_CARD_DIR/power_dpm_force_performance_level")
    if [[ $level != auto ]]; then
        printf 'b3: GPU clock level is already %s; leaving it alone\n' "$level"
        return 0
    fi
    if ! b3_gpu_clock_write power_dpm_force_performance_level manual ||
        ! b3_gpu_clock_write pp_od_clk_voltage "s 0 $mhz" ||
        ! b3_gpu_clock_write pp_od_clk_voltage "s 1 $mhz" ||
        ! b3_gpu_clock_write pp_od_clk_voltage c; then
        printf 'b3: could not set the GPU clock; running with the automatic clock\n' >&2
        b3_gpu_clock_release
        return 0
    fi
    printf 'b3: GPU clock held at %s MHz while the game runs\n' "$mhz"
    # setsid: Steam stops a game by signalling its process group, and the watcher has to outlive that.
    setsid bash -c 'trap "" HUP INT TERM; . "$0"; while kill -0 "$1" 2>/dev/null; do sleep 1; done; b3_gpu_clock_release' \
        "${BASH_SOURCE[0]}" "$pid" </dev/null >/dev/null 2>&1 &
    disown
}
