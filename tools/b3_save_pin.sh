#!/usr/bin/env bash
# Print where the pinned Prologue save states live and what they are for.
#
# These are game data and are never committed (.gitignore: logs/, private/). They are the one thing
# an experiment cannot rebuild: a save that reaches the Prologue fight. The 2026-10-27 round lost
# its copy to a disk cleanup and had to run the game again to find one, so read this before deleting
# anything under B3-data/ or logs/ch1/.
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_DATA=${B3_DATA:-$ROOT/private/B3-data}

printf 'Prologue save states (SlotData_0.dat md5 -> what it is)\n\n'
for d in "$B3_DATA/run/user/save.bak-20261023-1640" \
         "$ROOT/logs/ch1/battle-state/user" \
         "$ROOT/logs/ch1/battle-state-prologue" \
         "$B3_DATA/saves-pinned/prologue-original-10829c74" \
         "$ROOT/Bayonetta 3 Story Complete"; do
    [ -d "$d" ] || { printf '  %-62s (missing)\n' "$d"; continue; }
    m=$(md5 -q "$d/SlotData_0.dat" 2>/dev/null || echo '?')
    printf '  %-62s %s\n' "$d" "$m"
done
cat <<'TXT'

  10829c74f7ca3c42ef3f9155f51d7fdc  the Prologue save: the drive reaches the fight from it.
                                    scripts/b3_battle.sh defaults to SAVE_STATE=logs/ch1/battle-state/user.
  2295bf257710f45843aa989ac551bcbb  a progressed save that boots into the bar hub (Records of Time).
  b7c05702b150fd80d857a1a17e7c2aa5  the completed-story save (Bayonetta 3 Story Complete/).
  a205a885c748c0f44c05d296f1a3936e  a movie-run data root's save, not a pinned state.

Restore one before a run:
  mkdir -p <data root>/user/save && cp -a <state>/GameData.dat <state>/SlotData_0.dat <data root>/user/save/
TXT
