#!/usr/bin/env python3
"""Acceptance status: which commit has each platform actually verified?

WHY THIS FILE EXISTS
  logs/ is gitignored, so evidence never leaves the machine that produced it. A committed record under
  docs/acceptance/<platform>.json is the only acceptance evidence that crosses hosts. This tool reads those
  records, measures how far each one is behind HEAD, and turns "not verified here" / "verified twelve commits
  ago" into a queryable fact instead of a memory. scripts/b3_accept.sh --record is the writer; this is the
  reader and the gate.

  Read-only: it runs git and reads files. It never starts a game and never touches the network.

Usage:
  python3 tools/acceptance_status.py                       # table of every platform/profile
  python3 tools/acceptance_status.py --check               # gate: exit 1 unless everything is PASS at HEAD
  python3 tools/acceptance_status.py --check --max-lag 3
  python3 tools/acceptance_status.py --json
  python3 tools/acceptance_status.py --from-summary logs/mac/movie-attach14-summary.txt \
      --profile movie --commit 0d004e4 --write

Schema of docs/acceptance/<platform>.json (platform is b3-linux | b3-macos | b3-win | b3-android):
  {"platform": "b3-macos",
   "profiles": {"movie": {"commit": "<sha>", "date": "<ISO 8601>", "pass": true, "clean_env": true,
                          "stripped": ["NX_FOO=1"], "wall_seconds": 900, "fps_median": 55.0, "fills": 1454,
                          "fatal": 0, "crash": 0, "evidence": {"game_log": "logs/mac/x-game.log",
                          "summary": "logs/mac/x-summary.txt"}, "note": ""}}}
"""

import argparse
import datetime
import json
import re
import statistics
import subprocess
import sys
from pathlib import Path

# The platform ids are part of the contract: the file name is the platform id and the file is the record.
KNOWN_PLATFORMS = ("b3-linux", "b3-macos", "b3-win", "b3-android")
# NX_* variables a *default-environment* run is allowed to carry (mirrors B3_CLEAN_ENV_KEEP in
# scripts/lib/b3_common.sh). Anything else in a record's env is a diagnostic switch, so the run did not
# prove the default works.
KEEP_ENV = {"NX_DATA_ROOT", "NX_LOG", "NX_TMPDIR"}
DEFAULT_MAX_LAG = 0
PLATFORM_RE = re.compile(r"^b3-[a-z0-9]+$")
PROFILE_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
# A record names a commit, not a revision expression: only a hex sha reaches git.
SHA_RE = re.compile(r"^[0-9a-fA-F]{7,40}$")


# --- identity and git ----------------------------------------------------------------------------
def repo_root(override=None):
    if override:
        return Path(override).expanduser().resolve()
    return Path(__file__).resolve().parents[1]


def acceptance_dir(root):
    return root / "docs" / "acceptance"


def rel(root, path):
    """Path as a repository-relative string when possible, so the output is portable."""
    try:
        return Path(path).resolve().relative_to(Path(root).resolve()).as_posix()
    except (ValueError, OSError):
        return str(path)


def _git(root, *args):
    """Read-only git. Returns (returncode, stripped stdout). A missing git or repo is not fatal here."""
    try:
        proc = subprocess.run(["git", "-C", str(root)] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    except OSError:
        return 127, ""
    return proc.returncode, proc.stdout.strip()


def head_commit(root):
    rc, out = _git(root, "rev-parse", "HEAD")
    return out if rc == 0 and out else None


def head_short(root):
    rc, out = _git(root, "rev-parse", "--short", "HEAD")
    return out if rc == 0 and out else "unknown"


def head_branch(root):
    rc, out = _git(root, "rev-parse", "--abbrev-ref", "HEAD")
    return out if rc == 0 and out else "?"


def commit_known(root, commit):
    text = str(commit or "").strip()
    if not SHA_RE.match(text):
        return False
    return _git(root, "rev-parse", "--verify", "--quiet", text + "^{commit}")[0] == 0


def lag_info(root, commit):
    """How far a recorded commit is behind HEAD.

    The status is deliberately more than a number: a commit that no longer exists (history rewritten) or one
    that is not an ancestor of HEAD (work landed on a branch and never merged) cannot be counted in
    "commits behind", and guessing would hide exactly the drift this tool exists to expose.
    """
    text = str(commit or "").strip()
    info = {"commit": text, "commit_short": text[:7], "known": False, "ancestor": False,
            "behind": None, "status": "no-commit"}
    if not text or text == "unknown":
        return info
    if not commit_known(root, text):
        info["status"] = "unknown"
        return info
    info["known"] = True
    if _git(root, "merge-base", "--is-ancestor", text, "HEAD")[0] != 0:
        info["status"] = "not-ancestor"
        return info
    info["ancestor"] = True
    rc, out = _git(root, "rev-list", "--count", text + "..HEAD")
    if rc == 0 and out.isdigit():
        info["behind"] = int(out)
        info["status"] = "current" if info["behind"] == 0 else "behind"
    else:
        info["status"] = "unknown"
    return info


# --- records -------------------------------------------------------------------------------------
def platform_ids(root, requested=()):
    """Known platforms first, then any other b3-*.json someone added."""
    ids = list(KNOWN_PLATFORMS)
    adir = acceptance_dir(root)
    if adir.is_dir():
        for path in sorted(adir.glob("*.json")):
            if path.stem.startswith("b3-") and path.stem not in ids:
                ids.append(path.stem)
    for pid in requested:
        if pid not in ids:
            ids.append(pid)
    return ids


def load_records(root):
    """{platform: {profile: record}} plus file-level problems (unreadable/!JSON) as data, not exceptions."""
    records, problems = {}, []
    for pid in platform_ids(root):
        records[pid] = {}
        path = acceptance_dir(root) / (pid + ".json")
        if not path.is_file():
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            problems.append("%s: unreadable record file %s (%s)" % (pid, rel(root, path), exc))
            continue
        if not isinstance(data, dict):
            problems.append("%s: record file %s is not a JSON object" % (pid, rel(root, path)))
            continue
        profiles = data.get("profiles")
        if not isinstance(profiles, dict):
            problems.append("%s: record file %s has no 'profiles' object" % (pid, rel(root, path)))
            continue
        for name, rec in profiles.items():
            if isinstance(rec, dict):
                records[pid][name] = rec
            else:
                problems.append("%s/%s: record is not a JSON object" % (pid, name))
    return records, problems


def required_profiles(records, platforms, requested):
    if requested:
        return list(requested)
    union = []
    for pid in platforms:
        for name in records.get(pid, {}):
            if name not in union:
                union.append(name)
    return sorted(union)


def build_rows(root, records, platforms, profiles):
    """One row per required platform/profile, plus one MISSING row per platform with no record file."""
    rows = []
    for pid in platforms:
        present = records.get(pid, {})
        if not present:
            row = {"platform": pid, "profile": "-", "missing": True, "platform_missing": True}
            rows.append(row)
            continue
        for name in profiles:
            rec = present.get(name)
            if rec is None:
                rows.append({"platform": pid, "profile": name, "missing": True, "platform_missing": False})
                continue
            info = lag_info(root, rec.get("commit"))
            rows.append({
                "platform": pid,
                "profile": name,
                "missing": False,
                "commit": str(rec.get("commit") or ""),
                "commit_short": info["commit_short"],
                "date": rec.get("date") or "",
                "pass": rec.get("pass") is True,
                "clean_env": rec.get("clean_env"),
                "stripped": rec.get("stripped") or [],
                "wall_seconds": rec.get("wall_seconds"),
                "fps_median": rec.get("fps_median"),
                "fills": rec.get("fills"),
                "fatal": rec.get("fatal"),
                "crash": rec.get("crash"),
                "evidence": rec.get("evidence") or {},
                "note": rec.get("note") or "",
                "lag": info,
            })
    return rows


def evaluate(root, records, platforms, profiles, max_lag):
    """Return (problems, warnings). Problems are exactly the --check failure conditions."""
    problems, warnings = [], []
    head = head_commit(root)
    for pid in platforms:
        path = acceptance_dir(root) / (pid + ".json")
        if not path.is_file():
            problems.append("%s: no acceptance record (%s does not exist)" % (pid, rel(root, path)))
            continue
        present = records.get(pid, {})
        for name in profiles:
            rec = present.get(name)
            where = "%s/%s" % (pid, name)
            if rec is None:
                have = ", ".join(sorted(present)) or "nothing"
                problems.append("%s: no record (the file has: %s)" % (where, have))
                continue
            if rec.get("pass") is not True:
                problems.append("%s: recorded pass=%s" % (where, json.dumps(rec.get("pass"))))
            if rec.get("clean_env") is False:
                warnings.append("%s: recorded clean_env=false (%s); a run with diagnostic switches set "
                                "does not prove the default environment" %
                                (where, " ".join(rec.get("stripped") or []) or "switches recorded"))
            elif rec.get("clean_env") is None or "clean_env" not in rec:
                warnings.append("%s: clean_env unknown (%s); a replayed log or an imported summary does "
                                "not establish the environment - re-run scripts/b3_accept.sh --record "
                                "for a default-environment claim" %
                                (where, rec.get("note") or "no clean_env field"))
            info = lag_info(root, rec.get("commit"))
            if not head:
                problems.append("%s: cannot resolve HEAD (not a git checkout?)" % where)
            elif info["status"] == "no-commit":
                problems.append("%s: the record has no commit" % where)
            elif info["status"] == "unknown":
                problems.append("%s: recorded commit %s does not exist in this checkout (history rewritten?)"
                                % (where, info["commit_short"]))
            elif info["status"] == "not-ancestor":
                problems.append("%s: recorded commit %s is not an ancestor of HEAD (branched history)"
                                % (where, info["commit_short"]))
            elif info["behind"] is not None and info["behind"] > max_lag:
                problems.append("%s: behind HEAD by %d commit%s (%s..HEAD)"
                                % (where, info["behind"], "" if info["behind"] == 1 else "s",
                                   info["commit_short"]))
    return problems, warnings


# --- output --------------------------------------------------------------------------------------
def fmt_cell(value):
    if value is None:
        return "-"
    return str(value)


def print_table(root, rows, problems, warnings):
    head = head_short(root)
    branch = head_branch(root)
    print("HEAD %s (%s)" % (head, branch))
    print()
    headers = ("platform", "profile", "commit", "behind", "date", "pass")
    body = []
    for row in rows:
        if row.get("platform_missing"):
            body.append((row["platform"], "-", "-", "-", "-", "MISSING"))
            continue
        if row.get("missing"):
            body.append((row["platform"], row["profile"], "-", "-", "-", "MISSING"))
            continue
        lag = row["lag"]
        behind = lag["behind"] if lag["behind"] is not None else lag["status"]
        body.append((row["platform"], row["profile"], row["commit_short"] or "-",
                     fmt_cell(behind), (row["date"] or "")[:10] or "-",
                     "PASS" if row["pass"] else "FAIL"))
    widths = [len(h) for h in headers]
    for cells in body:
        for i, cell in enumerate(cells):
            widths[i] = max(widths[i], len(cell))
    line = "  ".join(h.ljust(widths[i]) for i, h in enumerate(headers))
    print(line)
    print("  ".join("-" * w for w in widths))
    for cells in body:
        print("  ".join(cell.ljust(widths[i]) for i, cell in enumerate(cells)))
    if not body:
        print("(no records)")
    for warning in warnings:
        print("warning: %s" % warning)
    if problems:
        print()
        print("%d problem(s); run with --check for the gate (exit code 1):" % len(problems))
        for problem in problems:
            print("  - %s" % problem)
    else:
        print()
        print("all required platforms are PASS at HEAD (lag 0) unless --max-lag was raised")


def to_json(root, rows, records, problems, warnings):
    head = head_commit(root)
    platforms = {}
    for pid, profiles in records.items():
        path = acceptance_dir(root) / (pid + ".json")
        platforms[pid] = {
            "file": rel(root, path),
            "exists": path.is_file(),
            "profiles": {},
        }
        for name, rec in profiles.items():
            enriched = dict(rec)
            enriched["lag"] = lag_info(root, rec.get("commit"))
            platforms[pid]["profiles"][name] = enriched
    return {
        "generated_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "repo": str(root),
        "head": head,
        "head_short": head_short(root),
        "branch": head_branch(root),
        "rows": rows,
        "platforms": platforms,
        "problems": problems,
        "warnings": warnings,
        "ok": not problems,
    }


# --- summary import ------------------------------------------------------------------------------
def _parse_seconds(text):
    match = re.search(r"([0-9]+(?:\.[0-9]+)?)", text or "")
    if not match:
        return None
    value = float(match.group(1))
    return int(value) if value.is_integer() else value


def _median_fps(lines):
    fps = []
    for line in lines:
        if "per frame over 60" not in line:
            continue
        match = re.search(r"per frame over 60:\s*([0-9]+(?:\.[0-9]+)?)\s*fps", line)
        if match:
            fps.append(float(match.group(1)))
    return round(statistics.median(fps), 1) if fps else None


# `movie: stopping <file>: 1458 luma + 1458 chroma plane fills` is the authoritative delivered count;
# the `luma plane fill #N` progress lines print only every 300 frames, so counting them is never right.
STOPPING_FILLS_RE = re.compile(
    r"movie: (?:stopping|replacing) .*?([0-9]+) luma \+[ 0-9]*chroma plane fills")


def _fill_count(lines):
    """Delivered luma frames, by the same criterion scripts/b3_accept.sh asserts.

    Prefer the closing `movie: stopping ... N luma ...` line. Older summaries have no closing line (the
    writer only kept the tail), so fall back to the highest `luma plane fill #N` ordinal, which is a lower
    bound of what the run delivered. The number of fill lines is never used as a count: a perfect
    1457-frame movie prints about five of them.
    """
    delivered = 0
    highest = 0
    for line in lines:
        match = STOPPING_FILLS_RE.search(line)
        if match:
            delivered = max(delivered, int(match.group(1)))
        if "luma plane fill" not in line:
            continue
        match = re.search(r"luma plane fill\s*#([0-9]+)", line)
        if match:
            highest = max(highest, int(match.group(1)))
    if delivered:
        return delivered
    return highest or None


def parse_summary(path):
    """Parse the summary format scripts/lib/b3_common.sh writes (and the older ad-hoc ones)."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    fields = {"name": "", "exe": "", "data_root": "", "commit": "", "platform": "", "env": ""}
    fatal = crash = 0
    wall = None
    for raw in lines:
        line = raw.strip()
        lowered = line.lower()
        handled = False
        for key, field in (("name:", "name"), ("exe:", "exe"), ("data root:", "data_root"),
                           ("commit:", "commit"), ("platform:", "platform"), ("env:", "env")):
            if lowered.startswith(key):
                fields[field] = line.split(":", 1)[1].strip()
                handled = True
                break
        if handled:
            continue
        if lowered.startswith("wall:"):
            wall = _parse_seconds(line.split(":", 1)[1])
        elif lowered.startswith("fatal lines:"):
            fatal = int(_parse_seconds(line.split(":", 1)[1]) or 0)
        elif lowered.startswith("crash lines:"):
            crash = int(_parse_seconds(line.split(":", 1)[1]) or 0)
    env = fields["env"]
    stripped = sorted({token.split("=", 1)[0] for token in env.split()
                       if re.match(r"^NX_[A-Za-z0-9_]+=", token)
                       and token.split("=", 1)[0] not in KEEP_ENV})
    return {
        "name": fields["name"],
        "exe": fields["exe"],
        "data_root": fields["data_root"],
        "commit": fields["commit"],
        "platform": fields["platform"],
        "env": env,
        "stripped": stripped,
        "clean_env": not stripped,
        "wall_seconds": wall,
        "fatal": fatal,
        "crash": crash,
        "fps_median": _median_fps(lines),
        "fills": _fill_count(lines),
    }


def infer_platform(summary_path, parsed):
    """Prefer the summary's own platform: line, then the log directory convention (logs/mac, logs/ch1)."""
    if parsed["platform"]:
        return parsed["platform"]
    parts = Path(summary_path).parts
    if "mac" in parts:
        return "b3-macos"
    if "ch1" in parts:
        return "b3-linux"
    return ""


def build_record_from_summary(args):
    summary_path = Path(args.from_summary).expanduser()
    if not summary_path.is_file():
        raise SystemExit("error: summary file not found: %s" % summary_path)
    if not args.profile:
        raise SystemExit("error: --from-summary needs --profile")
    if not PROFILE_RE.match(args.profile):
        raise SystemExit("error: invalid profile name: %r" % args.profile)
    parsed = parse_summary(summary_path)
    platform = args.platform or infer_platform(summary_path, parsed)
    if not platform:
        raise SystemExit("error: cannot infer the platform; pass --platform b3-linux|b3-macos")
    if not PLATFORM_RE.match(platform):
        raise SystemExit("error: invalid platform id: %r" % platform)
    commit = args.commit or parsed["commit"]
    if not commit or commit == "unknown":
        if args.write:
            raise SystemExit("error: no commit in the summary and no --commit given; a written record must "
                             "name the commit it verified")
        commit = "unknown"
    elif not SHA_RE.match(str(commit)):
        raise SystemExit("error: --commit must be a hex git sha, not %r" % (commit,))
    if args.date:
        date = args.date
    else:
        date = datetime.datetime.fromtimestamp(summary_path.stat().st_mtime).astimezone(
        ).isoformat(timespec="seconds")
    note = args.note or ""
    if not note:
        note = ("imported from %s; marker counts beyond the summary tail are not recoverable" %
                summary_path.name)
    record = {
        "commit": str(commit),
        "date": date,
        "pass": parsed["fatal"] == 0 and parsed["crash"] == 0 and bool(parsed["wall_seconds"]),
        "clean_env": parsed["clean_env"],
        "stripped": parsed["stripped"],
        "wall_seconds": parsed["wall_seconds"],
        "fps_median": parsed["fps_median"],
        "fills": parsed["fills"],
        "fatal": parsed["fatal"],
        "crash": parsed["crash"],
        "evidence": {"game_log": "", "summary": str(summary_path)},
        "note": note,
    }
    return platform, record


def write_record(root, platform, profile, record):
    path = acceptance_dir(root) / (platform + ".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise SystemExit("error: refusing to overwrite unreadable %s (%s)" % (path, exc))
        if not isinstance(data, dict):
            raise SystemExit("error: refusing to overwrite %s (not a JSON object)" % path)
    else:
        data = {}
    data["platform"] = platform
    profiles = data.get("profiles")
    if not isinstance(profiles, dict):
        profiles = {}
    profiles[profile] = record
    data["profiles"] = profiles
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


# --- command line --------------------------------------------------------------------------------
def split_list(value):
    return [item.strip() for item in (value or "").split(",") if item.strip()]


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Which commit has each platform last verified? Reads docs/acceptance/<platform>.json.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", default=None,
                        help="repository root (default: this checkout); useful for tests and other clones")
    parser.add_argument("--check", action="store_true",
                        help="gate: exit 1 when a record is missing, fails, or is behind HEAD")
    parser.add_argument("--max-lag", type=int, default=None, metavar="N",
                        help="allowed commits behind HEAD (default %d; implies --check)" % DEFAULT_MAX_LAG)
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--platforms", default=None, metavar="LIST",
                        help="comma-separated platform ids to require (default: known platforms + recorded ones)")
    parser.add_argument("--profiles", default=None, metavar="LIST",
                        help="comma-separated profiles to require (default: union of recorded profiles)")
    parser.add_argument("--from-summary", default=None, metavar="FILE",
                        help="import one existing run summary instead of reading records")
    parser.add_argument("--profile", default=None, help="profile name for --from-summary")
    parser.add_argument("--platform", default=None, help="platform id for --from-summary")
    parser.add_argument("--commit", default=None, help="commit verified by the summary")
    parser.add_argument("--date", default=None, help="ISO 8601 date for the record (default: summary mtime)")
    parser.add_argument("--note", default=None, help="free-text note for the record")
    parser.add_argument("--write", action="store_true",
                        help="with --from-summary: merge the record into docs/acceptance/<platform>.json")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root = repo_root(args.repo)
    if not root.is_dir():
        raise SystemExit("error: not a directory: %s" % root)

    if args.from_summary:
        platform, record = build_record_from_summary(args)
        if args.write:
            path = write_record(root, platform, args.profile, record)
            print(json.dumps(record, indent=2, sort_keys=True))
            print("wrote %s" % path, file=sys.stderr)
        else:
            print(json.dumps(record, indent=2, sort_keys=True))
        return 0

    max_lag = DEFAULT_MAX_LAG if args.max_lag is None else args.max_lag
    if max_lag < 0:
        raise SystemExit("error: --max-lag must be >= 0")
    check_mode = args.check or args.max_lag is not None

    requested_platforms = split_list(args.platforms)
    for pid in requested_platforms:
        if not PLATFORM_RE.match(pid):
            raise SystemExit("error: invalid platform id: %r" % pid)
    requested_profiles = split_list(args.profiles)
    for name in requested_profiles:
        if not PROFILE_RE.match(name):
            raise SystemExit("error: invalid profile name: %r" % name)

    platforms = platform_ids(root, requested_platforms) if not requested_platforms else requested_platforms
    records, load_problems = load_records(root)
    for pid in platforms:
        records.setdefault(pid, {})
    profiles = required_profiles(records, platforms, requested_profiles)
    rows = build_rows(root, records, platforms, profiles)
    problems, warnings = evaluate(root, records, platforms, profiles, max_lag)
    problems = load_problems + problems

    if args.json:
        payload = to_json(root, rows, records, problems, warnings)
        payload["max_lag"] = max_lag
        payload["check"] = check_mode
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print_table(root, rows, problems, warnings)
        if check_mode:
            print()
            if problems:
                print("check: FAILED (%d problem%s, max lag %d)"
                      % (len(problems), "" if len(problems) == 1 else "s", max_lag))
            else:
                print("check: OK (max lag %d)" % max_lag)
    if check_mode and problems:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
