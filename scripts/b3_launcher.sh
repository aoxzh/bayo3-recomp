#!/usr/bin/env bash
# The Bayonetta 3 launcher: settings, controller check and Play in one window.
#
#   scripts/b3_launcher.sh            # open it
#   scripts/b3_launcher.sh --play     # open it and start the game
#
# It edits private/b3.conf (the file scripts/b3_play.sh reads) and starts the game through that script, so a setting
# means the same thing here, in the file and in the environment. Tk is the only dependency: no python packages.
#
# The python is picked, not assumed: a toolchain python may ship a Tk with no font support (Tk 9 without Xft sees only
# the "fixed" bitmap font, which makes every label unreadable), while the system one has the usual font families. The
# first interpreter whose Tk can list fonts wins; B3_PYTHON overrides.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

usable_tk() {
    "$1" - <<'PY' >/dev/null 2>&1
import tkinter, tkinter.font as f
r = tkinter.Tk(); r.withdraw()
ok = len(f.families(r)) > 1
r.destroy()
raise SystemExit(0 if ok else 1)
PY
}

PY=${B3_PYTHON:-}
if [[ -z $PY ]]; then
    for candidate in /usr/bin/python3 python3; do
        command -v "$candidate" >/dev/null 2>&1 || continue
        if usable_tk "$candidate"; then PY=$candidate; break; fi
    done
fi
PY=${PY:-python3}
exec "$PY" "$ROOT/tools/b3_launcher.py" "$@"
