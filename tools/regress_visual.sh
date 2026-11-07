#!/usr/bin/env bash
# Cross-platform entry point for the visual regression (macOS / Linux / Steam Deck), no PowerShell needed.
#
# Same commands and exit codes as tools/visual_regress.py:
#   tools/regress_visual.sh capture [--exe <game>] [--data-root <NX_DATA_ROOT>] [--label <platform>] ...
#   tools/regress_visual.sh check   [--frames <frames dir>] [--label <platform>] [--allow-resize] ...
#   tools/regress_visual.sh bless   --from <frames dir or goldens dir> [--note "why"] ...
#   tools/regress_visual.sh verify  [--exe <game>] [--data-root <NX_DATA_ROOT>] [--label ...] ...
#   tools/regress_visual.sh selftest
#
# Exit codes: 0 pass or skipped (no goldens for the label), 1 visual regression, 2 usage/config error.
# Goldens are written under private/bayonetta3/visual/<label>/ (gitignored). One golden set is valid only for the
# GPU/driver/OS of its label; capture again on another platform instead of copying a set.
#
# Steam Deck (Linux/Proton or native build) in three commands:
#   ./tools/regress_visual.sh selftest
#   NX_DATA_ROOT="$PWD/private/run-bayonetta3" ./tools/regress_visual.sh capture --exe "$PWD/private/build/bayonetta3" \
#       --label linux-amdgpu --seconds 240 --adopt
#   NX_DATA_ROOT="$PWD/private/run-bayonetta3" ./tools/regress_visual.sh check --label linux-amdgpu
#
# Environment:
#   VISUAL_PYTHON / PYTHON   python with Pillow (default: python3)
#   NX_VISUAL_LABEL          default label (macos-moltenvk / windows / linux-<driver>)
#   NX_DATA_ROOT             default data root for capture/verify/check --frames
#   NX_VISUAL_GOLDENS        alternate goldens root (default private/bayonetta3/visual)
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
TOOL="$ROOT/tools/visual_regress.py"
PY="${VISUAL_PYTHON:-${PYTHON:-python3}}"

usage() {
    sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

if [ ! -f "$TOOL" ]; then
    echo "regress_visual.sh: $TOOL not found (run from a full checkout)" >&2
    exit 2
fi

cmd="${1:-}"
if [ $# -gt 0 ]; then shift; fi

case "$cmd" in
    capture|verify)
        exec "$PY" "$TOOL" "$cmd" "$@"
        ;;
    check)
        args=("$@")
        has_frames=0
        for a in "$@"; do
            case "$a" in --frames|--frames=*) has_frames=1 ;; esac
        done
        if [ "$has_frames" -eq 0 ] && [ -n "${NX_DATA_ROOT:-}" ]; then
            args+=(--frames "$NX_DATA_ROOT/frames")
        fi
        exec "$PY" "$TOOL" check "${args[@]}"
        ;;
    bless)
        exec "$PY" "$TOOL" bless "$@"
        ;;
    selftest)
        exec "$PY" "$TOOL" selftest "$@"
        ;;
    --selftest)
        exec "$PY" "$TOOL" --selftest "$@"
        ;;
    ""|-h|--help|help)
        usage
        exit 2
        ;;
    *)
        echo "regress_visual.sh: unknown command '$cmd'" >&2
        usage >&2
        exit 2
        ;;
esac
