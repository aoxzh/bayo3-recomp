#!/usr/bin/env bash
# Default-environment acceptance run for one profile on one host: the executor of the rule "a fix
# must be on with no switch set".
#
#   scripts/b3_accept.sh --profile <boot|title|movie|hub> [--record] [--dry-run]
#                        [--platform b3-linux|b3-macos|b3-win] [--exe PATH] [--data-root DIR]
#                        [--timeout SEC] [--name NAME] [KEY=VALUE ...]
#
# Why it exists (docs/CROSS-PLATFORM.md): a repair that only ran when a diagnostic switch was set
# (ev0005's attach, commit 0d004e4) was read as verified, and each host's entry script had grown its
# own parameters. So an acceptance here strips every diagnostic NX_* switch, runs the profile through
# that platform's own entry script (the code path a player actually uses) and asserts markers that
# cannot appear unless the repair is on by default.
#
# Profiles (window / what is asserted):
#   boot   300 s, no driving: the modules link (0 unresolved) and the host presents frames
#   title  400 s driven: the same, after the driver has walked the title screens
#   movie  900 s driven: `movie-vt: attached movie system` and >=1000 `movie: ... luma plane fill`
#   hub    900 s driven, driver takes YES: the save/hub path reaches the book (`am: ExecuteProgram` /
#          `am: ClearUserChannel`) or, on the dartboard path, a chapter start (see the note above
#          `profile_min_markers`); the movie chain is still attached
# Any marker below its floor, or any FATAL/CRASH line, is a fail: exit code 1.
#
# --dry-run prints the entry command, the variables a clean run would strip and the markers that
# would be asserted, then exits 0 without launching anything (B3_DRY_RUN=1 does the same).
# --from-log FILE asserts against a log an earlier run produced: no strip, no launch. It exists so the
# marker table can be tested (and old evidence re-judged) on a host that must not start the game.
# --record merges the result into docs/acceptance/<platform>.json: commit, date, pass, the stripped
# variable names, counts and local log paths only - never game data.
#
# KEY=VALUE switches are refused: they are not a default-environment run. B3_CLEAN_ENV=0 allows them
# and records clean_env=false.
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
B3_ROOT=$ROOT
. "$ROOT/scripts/lib/b3_common.sh"

TAB=$(printf '\t')

usage() {
    cat <<'EOF'
usage: scripts/b3_accept.sh --profile <boot|title|movie|hub> [--record] [--dry-run]
                            [--platform b3-linux|b3-macos|b3-win] [--exe PATH] [--data-root DIR]
                            [--timeout SEC] [--name NAME] [KEY=VALUE ...]

  --profile   boot | title | movie | hub   (required)
  --record    merge the result into docs/acceptance/<platform>.json
  --dry-run   print the plan and exit 0 (same as B3_DRY_RUN=1)
  --from-log  assert against an existing game log instead of running anything
              (re-check historical evidence, or test the markers without a GPU)
  --platform  b3-linux (default on Linux) | b3-macos (default on Darwin) | b3-win (default on Windows)
  --exe       game binary (default: the platform entry script's default)
  --data-root NX_DATA_ROOT (default: the platform entry script's default)
  --timeout   override the profile's window in seconds
  --name      run name (default accept-<profile>); the logs are <logs>/<name>-*.log

The profiles need different save states and cannot share one data root:

  boot        any root
  movie       a root whose save has NOT played the intro movies yet (a progressed save drives
              straight into the Prologue scene and the movie markers can never appear)
  hub         a root whose save has COMPLETED the story: the driver should reach the bar, the book
              (`am: ExecuteProgram`) OR a chapter start (dartboard select; see the note above the marker table)

A clean-env run takes no KEY=VALUE switches; B3_CLEAN_ENV=0 allows them and records clean_env=false.
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 2; }

rel_to_root() {
    case "$1" in
        "$ROOT"/*) printf '%s' "${1#"$ROOT"/}" ;;
        *)         printf '%s' "$1" ;;
    esac
}

# --- profiles ------------------------------------------------------------------------------------
profile_seconds() {
    case "$1" in
        boot)  printf '300' ;;
        title) printf '400' ;;
        movie) printf '900' ;;
        hub)   printf '900' ;;
    esac
}
profile_drives() {
    # 0 = the entry script does not start the driver (boot only).
    case "$1" in
        boot) printf '0' ;;
        *)    printf '1' ;;
    esac
}
profile_take_yes() {
    case "$1" in
        hub) printf '1' ;;
        *)   printf '0' ;;
    esac
}
profile_min_markers() {
    # <minimum count> TAB <extended regex>; the same table drives the plan and the assertion.
    printf '%s\t%s\n' 1 'linked: .* 0 unresolved'
    printf '%s\t%s\n' 10 'per frame over 60'
    # The movie marker is platform-specific: the desktop POSIX hosts decode through a forked ffmpeg and the movie
    # system's vtable patch logs "movie-vt: attached movie system"; Windows uses Media Foundation (movie_win.cpp),
    # which needs no vtable repair, so it logs the decoder it opened instead. Accept either.
    case "$1" in
        movie)
            printf '%s\t%s\n' 1 'movie-vt: attached movie system|movie: .* -> Media Foundation|movie: .* luma plane fill'
            ;;
        hub)
            printf '%s\t%s\n' 1 'movie-vt: attached movie system|movie: .* -> Media Foundation|movie: .* luma plane fill'
            printf '%s\t%s\n' 1 'am: ExecuteProgram|am: ClearUserChannel'
            ;;
    esac
}

# NOTE (2026-11-03, docs/CHAPTER1-AUTODRIVE.md §12.14b): the `hub` marker above is a *book-path* marker. Reaching a
# chapter does NOT need prog1: from the bar, hold L to enter aim, put the reticle on a purple node of the dartboard
# world map, press A (Chapter Select), A (checkpoint), A (stage). That path logs no `am:` line at all, so an
# auto-driver run that selects a chapter this way can still FAIL this marker. Until the driver learns the aim +
# dartboard gesture, `hub` should be treated as "walked to the book" rather than "reached a chapter"; the
# chapter-selection claim is carried by the manual evidence in logs/win/probe-chapter8-2026-11-03/.

profile_min_fills() {
    # The log prints `movie: stopping <file>: 1458 luma + 1458 chroma plane fills`, and a per-300-frame
    # `luma plane fill #N` progress line. The acceptance wants the former: how many frames a movie
    # actually delivered, not how many lines the log happened to print (a perfect 1457-frame movie
    # prints ~5 progress lines, so a line count is not a criterion).
    case "$1" in
        movie) printf '1000' ;;
        hub)   printf '100' ;;
        *)     printf '' ;;
    esac
}

max_luma_fills() {
    grep -E 'movie: (stopping|replacing) ' "$1" 2>/dev/null \
        | sed -E 's/.* ([0-9]+) luma \+[ 0-9]*chroma plane fills.*/\1/' \
        | grep -E '^[0-9]+$' | sort -n | tail -1
}

# --- arguments -----------------------------------------------------------------------------------
PROFILE=
PLATFORM_ARG=
EXE_ARG=
DATA_ARG=
TIMEOUT_ARG=
NAME=
RECORD=0
DRY=0
FROM_LOG=
SWITCHES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --profile)   [ $# -ge 2 ] || die "--profile needs a value";   PROFILE=$2; shift 2 ;;
        --platform)  [ $# -ge 2 ] || die "--platform needs a value";  PLATFORM_ARG=$2; shift 2 ;;
        --exe)       [ $# -ge 2 ] || die "--exe needs a value";       EXE_ARG=$2; shift 2 ;;
        --data-root) [ $# -ge 2 ] || die "--data-root needs a value"; DATA_ARG=$2; shift 2 ;;
        --timeout)   [ $# -ge 2 ] || die "--timeout needs a value";   TIMEOUT_ARG=$2; shift 2 ;;
        --name)      [ $# -ge 2 ] || die "--name needs a value";      NAME=$2; shift 2 ;;
        --record)    RECORD=1; shift ;;
        --dry-run)   DRY=1; shift ;;
        --from-log)  [ $# -ge 2 ] || die "--from-log needs a file"; FROM_LOG=$2; shift 2 ;;
        -h|--help)   usage; exit 0 ;;
        -*)          die "unknown option $1" ;;
        *)           SWITCHES+=("$1"); shift ;;
    esac
done

case "$PROFILE" in
    boot|title|movie|hub) ;;
    "")  die "--profile is required (boot|title|movie|hub)" ;;
    *)   die "unknown profile '$PROFILE' (boot|title|movie|hub)" ;;
esac

PLATFORM=${PLATFORM_ARG:-$(b3_platform)}
case "$PLATFORM" in
    b3-linux|b3-macos|b3-win) ;;
    *) die "unsupported platform '$PLATFORM' (b3-linux|b3-macos|b3-win)" ;;
esac
export B3_PLATFORM="$PLATFORM"

if b3_dry_run; then DRY=1; fi

if [ -n "$FROM_LOG" ] && [ "$RECORD" = "1" ]; then
    cat >&2 <<'EOM'
error: --from-log only judges an existing log; it cannot produce an acceptance record.

  A record answers "this platform verified this commit in the default environment". A replay has no
  run, no commit and no environment, so recording it would be a false claim. Drop --record to check
  the log, or run the profile live (no --from-log) to record it.
EOM
    exit 2
fi

# --- clean environment ---------------------------------------------------------------------------
CLEAN_ENV=1
if [ "${B3_CLEAN_ENV:-1}" = "0" ]; then CLEAN_ENV=0; fi
if [ "$CLEAN_ENV" = "1" ] && [ ${#SWITCHES[@]} -gt 0 ]; then
    printf 'error: a clean-env acceptance takes no KEY=VALUE switches: %s\n' "${SWITCHES[*]}" >&2
    printf '       set B3_CLEAN_ENV=0 to run with them (the record then says clean_env=false)\n' >&2
    exit 2
fi

STRIP_LOG=$(mktemp "${TMPDIR:-/tmp}/b3accept-strip.XXXXXX") || die "mktemp failed"
trap 'rm -f "$STRIP_LOG"' EXIT

STRIPPED_JOINED=""
if [ -n "$FROM_LOG" ]; then
    printf 'stripped: n/a (--from-log replays an existing run; it cannot change its environment)\n'
elif [ "$CLEAN_ENV" = "1" ]; then
    # Strip in *this* shell: a command substitution would unset the variables in the subshell only,
    # and the entry script would still inherit them.
    b3_strip_diagnostic_env > "$STRIP_LOG"
    if [ -s "$STRIP_LOG" ]; then
        STRIPPED_JOINED=$(sed 's/^stripped: //' "$STRIP_LOG")
        printf '%s\n' "$STRIPPED_JOINED" | sed 's/^/stripped: /'
    else
        printf 'stripped: none (no diagnostic NX_* switch was set)\n'
    fi
fi
STRIPPED_COUNT=0
if [ -n "$STRIPPED_JOINED" ]; then
    STRIPPED_COUNT=$(printf '%s\n' "$STRIPPED_JOINED" | wc -l | tr -d ' ')
fi

# --- what to run ---------------------------------------------------------------------------------
DATA_ROOT=${DATA_ARG:-$(b3_default_data_root)}
EXE=${EXE_ARG:-$(b3_default_exe)}
LOGS=$(b3_default_logs)
NAME=${NAME:-accept-$PROFILE}
GAME_LOG=$LOGS/$NAME-game.log
DRIVE_LOG=$LOGS/$NAME-drive.log
SUMMARY=$LOGS/$NAME-summary.txt
RECORD_PATH=$ROOT/docs/acceptance/$PLATFORM.json

b3_export_base_env "$DATA_ROOT"

PROFILE_SECS=$(profile_seconds "$PROFILE")
DRIVES=$(profile_drives "$PROFILE")
TAKE=$(profile_take_yes "$PROFILE")
if [ -n "$TIMEOUT_ARG" ]; then PROFILE_SECS=$TIMEOUT_ARG; fi

# The acceptance is the *measurement* environment, and that environment is headless. A windowed run is a
# visual-evidence mode (b3_experiment.sh HEADED=1): if a caller's HEADED=1 leaked into the entry script the
# record would claim clean_env=1 for a run whose frame path differs, so pin it off here.
if [ "$PLATFORM" = "b3-macos" ]; then
    ENTRY=$ROOT/scripts/b3_mac_run.sh
    # The mac entry takes a total run window and drives for the same time; keep a tail for evidence.
    RUN_SECONDS=$PROFILE_SECS
    if [ "$DRIVES" = "1" ]; then POST_SECONDS=60; else POST_SECONDS=0; fi
    entry_pos=("$NAME" "$RUN_SECONDS")
    entry_env=(B3_PLATFORM="$PLATFORM" EXE="$EXE" DATA_ROOT="$DATA_ROOT" LOGS="$LOGS" \
               AUTODRIVE="$DRIVES" EVERY=1.5 TAKE_YES="$TAKE" FRAMES=0 PRE=0 POST="$POST_SECONDS")
    WINDOW="$RUN_SECONDS s run + $POST_SECONDS s post"
elif [ "$PLATFORM" = "b3-win" ]; then
    # The Windows entry has the mac's shape (a total window, driving for the same time), but the
    # acceptance is the headless measurement: WINDOWED=0 keeps a console window out of the run, since a
    # DWM-composited window can pace the swapchain differently from present to present.
    ENTRY=$ROOT/scripts/b3_win_run.sh
    RUN_SECONDS=$PROFILE_SECS
    if [ "$DRIVES" = "1" ]; then POST_SECONDS=60; else POST_SECONDS=0; fi
    entry_pos=("$NAME" "$RUN_SECONDS")
    entry_env=(B3_PLATFORM="$PLATFORM" EXE="$EXE" DATA_ROOT="$DATA_ROOT" LOGS="$LOGS" \
               AUTODRIVE="$DRIVES" EVERY=1.5 TAKE_YES="$TAKE" PRE=0 POST="$POST_SECONDS" WINDOWED=0)
    WINDOW="$RUN_SECONDS s run + $POST_SECONDS s post"
else
    ENTRY=$ROOT/scripts/b3_experiment.sh
    # The Deck entry takes the drive window; boot drives for 0 s and keeps the game up instead.
    if [ "$DRIVES" = "1" ]; then
        DRIVE_SECONDS_ARG=$PROFILE_SECS
        POST_SECONDS=60
    else
        DRIVE_SECONDS_ARG=0
        POST_SECONDS=$PROFILE_SECS
    fi
    entry_pos=("$NAME" "$DRIVE_SECONDS_ARG")
    entry_env=(B3_PLATFORM="$PLATFORM" EXE="$EXE" DATA_ROOT="$DATA_ROOT" LOGS="$LOGS" \
               POST="$POST_SECONDS" EVERY=1.5 A_ONLY=0 TAKE_YES="$TAKE" PERF=0 HEADED=0)
    WINDOW="$DRIVE_SECONDS_ARG s drive + $POST_SECONDS s post"
fi
if [ "$CLEAN_ENV" = "0" ] && [ ${#SWITCHES[@]} -gt 0 ]; then
    entry_pos+=("${SWITCHES[@]}")
fi

[ -f "$ENTRY" ] || die "missing entry script $ENTRY"

NOTE="profile $PROFILE via $(basename "$ENTRY"), $WINDOW"
if [ "$CLEAN_ENV" = "0" ]; then
    NOTE="$NOTE; clean_env=0"
    if [ ${#SWITCHES[@]} -gt 0 ]; then NOTE="$NOTE switches: ${SWITCHES[*]}"; fi
fi

COMMIT=$(b3_git_commit)

# --- dry run: print the plan, launch nothing -----------------------------------------------------
if [ "$DRY" = "1" ]; then
    {
        printf 'accept: %s profile %s on %s\n' "$ROOT/scripts/b3_accept.sh" "$PROFILE" "$PLATFORM"
        printf 'commit: %s\n' "$COMMIT"
        printf 'name: %s\n' "$NAME"
        printf 'clean env: %s (stripped %s variable(s))\n' "$CLEAN_ENV" "$STRIPPED_COUNT"
        printf 'window: %s\n' "$WINDOW"
        printf 'entry: %s\n' "$ENTRY"
        printf 'entry env: %s\n' "${entry_env[*]}"
        printf 'entry argv: %s %s\n' "$ENTRY" "${entry_pos[*]}"
        printf 'execute: env %s %s %s\n' "${entry_env[*]}" "$ENTRY" "${entry_pos[*]}"
        printf 'game log: %s\n' "$GAME_LOG"
        printf 'summary: %s\n' "$SUMMARY"
        printf 'record: %s (written only with --record)\n' "$RECORD_PATH"
        while IFS="$TAB" read -r min pat; do
            printf 'marker: %s >= %s\n' "$pat" "$min"
        done < <(profile_min_markers "$PROFILE")
        printf 'marker: FATAL == 0\n'
        printf 'marker: CRASH == 0\n'
    } | b3_dry_run_report
    exit 0
fi

# --- run through the platform's own entry script, or replay an existing log -----------------------
if [ -n "$FROM_LOG" ]; then
    GAME_LOG=$FROM_LOG
    [ -f "$GAME_LOG" ] || die "--from-log: no such file $GAME_LOG"
    SUMMARY=""
    ENTRY_RC=0
    WALL=0
    NOTE="profile $PROFILE from-log $(rel_to_root "$GAME_LOG"); replayed log, clean_env unknown"
    printf 'acceptance: profile %s on %s, replayed from %s\n' "$PROFILE" "$PLATFORM" "$GAME_LOG"
else
    [ -x "$EXE" ] || die "no executable $EXE (pass --exe)"
    mkdir -p "$LOGS"
    # Never judge a run against a log an earlier run left behind.
    rm -f "$GAME_LOG" "$DRIVE_LOG" "$SUMMARY"

    printf 'acceptance: profile %s on %s, name %s, clean_env=%s\n' "$PROFILE" "$PLATFORM" "$NAME" "$CLEAN_ENV"
    printf 'entry: %s\n' "$ENTRY"
    START=$(date +%s)
    env "${entry_env[@]}" "$ENTRY" "${entry_pos[@]}"
    ENTRY_RC=$?
    WALL=$(( $(date +%s) - START ))
    printf 'entry exit %s after %s s\n' "$ENTRY_RC" "$WALL"
fi

# --- assertions ----------------------------------------------------------------------------------
CHECKS_FAILED=0

check_min() {  # <extended regex> <minimum count>
    local pattern=$1 min=$2 n
    n=$(grep -cE "$pattern" "$GAME_LOG" 2>/dev/null || true)
    n=${n:-0}
    if [ "$n" -ge "$min" ]; then
        printf 'PASS %-42s %6s >= %-6s\n' "$pattern" "$n" "$min"
    else
        printf 'FAIL %-42s %6s <  %-6s\n' "$pattern" "$n" "$min"
        CHECKS_FAILED=1
    fi
}

check_min_number() {  # <label> <value> <minimum>
    local label=$1 value=$2 min=$3
    if [ "${value:-0}" -ge "$min" ]; then
        printf 'PASS %-42s %6s >= %-6s\n' "$label" "$value" "$min"
    else
        printf 'FAIL %-42s %6s <  %-6s\n' "$label" "${value:-0}" "$min"
        CHECKS_FAILED=1
    fi
}

check_zero() {  # <label> <extended regex>
    local label=$1 pattern=$2 n
    n=$(grep -cE "$pattern" "$GAME_LOG" 2>/dev/null || true)
    n=${n:-0}
    if [ "$n" -eq 0 ]; then
        printf 'PASS %-42s %6s == 0\n' "$label" "$n"
    else
        printf 'FAIL %-42s %6s >  0\n' "$label" "$n"
        CHECKS_FAILED=1
    fi
}

if [ "$ENTRY_RC" -ne 0 ]; then
    printf 'FAIL %-42s entry exit %s\n' 'entry script' "$ENTRY_RC"
    CHECKS_FAILED=1
fi
if [ -f "$GAME_LOG" ]; then
    # A `movie` run against a save that already played the intro never touches Contents:/movie/: the
    # game resumes into the Prologue scene instead. That produces the same FAIL lines as a broken
    # movie repair, which sent this session chasing the wrong thing twice, so say which one it is.
    if [ "$PROFILE" = "movie" ]; then
        MOVIE_OPENS=$(grep -cE 'Contents:/movie/[^ ]+\.webm' "$GAME_LOG" 2>/dev/null || true)
        if [ "${MOVIE_OPENS:-0}" -eq 0 ]; then
            cat >&2 <<'EOM'
note: this run never opened a movie file, so the movie markers below cannot pass. That is the save
      state, not the platform: a save that is already past the intro resumes into the Prologue scene.
      Re-run against a fresh data root, e.g.
          scripts/b3_accept.sh --profile movie --record --data-root private/run-movie
      (the entry script bootstraps an empty root itself; `hub` is the profile that wants the
      completed save, `movie` wants the unstarted one).
EOM
        fi
    fi
    while IFS="$TAB" read -r min pat; do
        check_min "$pat" "$min"
    done < <(profile_min_markers "$PROFILE")
    MIN_FILLS=$(profile_min_fills "$PROFILE")
    if [ -n "$MIN_FILLS" ]; then
        FILLS_SEEN=$(max_luma_fills "$GAME_LOG")
        check_min_number 'max movie luma fills' "${FILLS_SEEN:-0}" "$MIN_FILLS"
    fi
    check_zero 'FATAL' 'FATAL'
    check_zero 'CRASH' 'CRASH'
else
    printf 'FAIL %-42s no game log at %s\n' 'game log' "$GAME_LOG"
    CHECKS_FAILED=1
fi

# --- counts for the record (numbers and paths only) ----------------------------------------------
count_lines() { grep -cE "$1" "$GAME_LOG" 2>/dev/null || true; }
FATAL_N=$(count_lines 'FATAL'); FATAL_N=${FATAL_N:-0}
CRASH_N=$(count_lines 'CRASH'); CRASH_N=${CRASH_N:-0}
FILLS_N=$(max_luma_fills "$GAME_LOG"); FILLS_N=${FILLS_N:-0}
FPS_VALUE=""
if [ -f "$GAME_LOG" ]; then
    FPS_VALUE=$("$(b3_python)" - "$GAME_LOG" <<'PY'
import re, statistics, sys
rows = []
try:
    with open(sys.argv[1], errors="replace") as handle:
        for line in handle:
            if "per frame over 60" not in line:
                continue
            match = re.search(r"([\d.]+) fps", line)
            if match:
                rows.append(float(match.group(1)))
except OSError:
    pass
print("%.1f" % statistics.median(rows) if rows else "")
PY
)
fi

# A replay never establishes the environment, so it must not print clean_env=1.
if [ -n "$FROM_LOG" ]; then CLEAN_ENV_LABEL=unknown; else CLEAN_ENV_LABEL=$CLEAN_ENV; fi
if [ "$CHECKS_FAILED" -eq 0 ]; then
    PASS=true
    printf 'acceptance PASS: %s profile %s (%s s, clean_env=%s)\n' \
        "$PLATFORM" "$PROFILE" "$WALL" "$CLEAN_ENV_LABEL"
else
    PASS=false
    printf 'acceptance FAIL: %s profile %s (%s s, clean_env=%s) - see %s\n' \
        "$PLATFORM" "$PROFILE" "$WALL" "$CLEAN_ENV_LABEL" "$GAME_LOG"
fi

# --- record --------------------------------------------------------------------------------------

if [ "$RECORD" = "1" ]; then
    if [ -n "$FROM_LOG" ]; then
        # --from-log replays a log; it cannot re-establish the environment that produced it, so the
        # honest value is "unknown" (JSON null), never true: a record must not claim a default run it
        # did not make.
        CLEAN_ENV_BOOL=null
    else
        CLEAN_ENV_BOOL=$([ "$CLEAN_ENV" = "1" ] && echo true || echo false)
    fi
    DATE_ISO=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    "$(b3_python)" - "$RECORD_PATH" \
        "platform=$PLATFORM" "profile=$PROFILE" "commit=$COMMIT" "date=$DATE_ISO" \
        "pass=$PASS" "clean_env=$CLEAN_ENV_BOOL" "wall=$WALL" "fps=$FPS_VALUE" \
        "fills=$FILLS_N" "fatal=$FATAL_N" "crash=$CRASH_N" \
        "game_log=$(rel_to_root "$GAME_LOG")" "summary=$(rel_to_root "$SUMMARY")" \
        "note=$NOTE" "stripped=$STRIPPED_JOINED" \
        "data_root=$( [ -n "$FROM_LOG" ] && echo "" || rel_to_root "$DATA_ROOT" )" <<'PY'
import json
import os
import sys

path = sys.argv[1]
fields = {}
for arg in sys.argv[2:]:
    key, _, value = arg.partition("=")
    fields[key] = value


def number(name, cast):
    value = fields.get(name, "")
    return cast(value) if value not in ("", None) else None


profile = {
    "commit": fields.get("commit", ""),
    "date": fields.get("date", ""),
    "pass": fields.get("pass") == "true",
    # true = a live default-environment run; false = a live run with switches; null = replayed log
    # (unknown environment). Never collapse "unknown" into true.
    "clean_env": (None if fields.get("clean_env") == "null"
                  else fields.get("clean_env") == "true"),
    "stripped": [line for line in fields.get("stripped", "").splitlines() if line],
    "wall_seconds": number("wall", int),
    "fps_median": number("fps", float),
    "fills": number("fills", int) or 0,
    "fatal": number("fatal", int) or 0,
    "crash": number("crash", int) or 0,
    "evidence": {
        "game_log": fields.get("game_log", ""),
        "summary": fields.get("summary", ""),
    },
    "note": fields.get("note", ""),
    # Which data root produced this number. Empty for a replayed log. A `movie` PASS on a scratch root
    # and one on the default root are different claims, and the reviewer has to be able to tell them
    # apart (docs/archive/SESSION-2026-10-26-MAC.md §3b).
    "data_root": fields.get("data_root", ""),
}

data = {}
if os.path.exists(path):
    try:
        with open(path, encoding="utf-8") as handle:
            data = json.load(handle)
    except ValueError as exc:
        raise SystemExit("%s: refusing to merge into invalid JSON: %s" % (path, exc))
    if not isinstance(data, dict):
        raise SystemExit("%s: not a JSON object" % path)

data.setdefault("platform", fields.get("platform", ""))
profiles = data.setdefault("profiles", {})
profiles[fields["profile"]] = profile

os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
with open(path, "w", encoding="utf-8") as handle:
    json.dump(data, handle, indent=2, sort_keys=True)
    handle.write("\n")
print("recorded %s profile %s" % (path, fields["profile"]))
PY
fi

[ "$CHECKS_FAILED" -eq 0 ] && exit 0
exit 1
