#!/usr/bin/env python3
"""Contract tests for the cross-platform run harness (scripts/lib/b3_common.sh).

WHY THESE CHECKS EXIST
  Every waste of a cross-platform session has one of three shapes: a driver invocation grown by hand on one
  host (--game-log passed on macOS but not on Deck), an entry point that quietly stopped using the shared
  library, or bash-4 syntax that only breaks on the macOS host. The static checks below make each shape a
  test failure instead of a lost evening. The dynamic checks use B3_DRY_RUN=1: the entry points assemble
  every command and print it with a B3_DRY_RUN: prefix instead of starting a game, so the whole contract is
  testable on a machine with no GPU and without touching the real data root.

  The acceptance-status gate is exercised in a throwaway git repository, never in this checkout.

Run: python3 tests/test_cross_platform_harness.py     (ctest name: cross_platform_harness)
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
LIB = SCRIPTS / "lib" / "b3_common.sh"
TOOL = ROOT / "tools" / "acceptance_status.py"

# Every entry point must source the shared contract (scripts/b3_accept.sh lands with the entrypoint task).
ENTRY_POINTS = ("scripts/b3_experiment.sh", "scripts/b3_mac_run.sh", "scripts/b3_accept.sh")
# Scripts that have to run under macOS /bin/bash 3.2: the macOS entry point, its build lock partner, the
# acceptance runner, preflight, and the shared library they all source.
MACOS_SHARED = ("scripts/b3_mac_run.sh", "scripts/b3_mac_build.sh", "scripts/b3_accept.sh",
                "scripts/b3_preflight.sh", "scripts/lib/b3_common.sh")
# Flags b3_driver_args owns. An entry point spelling one of these out by hand is the drift this file catches.
FORBIDDEN_DRIVER_FLAGS = ("--game-log", "--a-only-until", "--take-yes", "--movie-")
BASH4_PATTERNS = (
    (re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*,,}?"), "${var,,}"),
    (re.compile(r"\$\{[A-Za-z_][A-Za-z0-9_]*\^\^}?"), "${var^^}"),
    (re.compile(r"\bdeclare\s+-[aA]\b"), "declare -A"),
    (re.compile(r"\b(?:mapfile|readarray)\b"), "mapfile/readarray"),
)
PY = sys.executable


class Skip(Exception):
    """A check that cannot run yet, reported as SKIP instead of failing the suite."""


RESULTS = []


def _win_dirs():
    r"""PATH entries as Windows paths.

    Inside Git Bash, PATH can hold POSIX spellings (`/usr/bin`, `/c/Tools/...`) that no Win32 call can resolve, so
    rewrite `/c/...` to `C:\...` and keep both spellings of every entry.
    """
    out = []
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        entry = entry.strip().strip('"')
        if not entry:
            continue
        out.append(entry)
        if re.match(r"^/[A-Za-z]/", entry):
            out.append(entry[1].upper() + ":" + entry[2:].replace("/", "\\"))
    return out


def _bash_candidates():
    """Every plausible bash.exe, best first. `bash` on PATH is not it on Windows (see _resolve_bash)."""
    out = [os.environ.get("B3_BASH")]
    out += ["bash", "bash.exe", "/usr/bin/bash", "/bin/bash"]
    for directory in _win_dirs():
        out.append(os.path.join(directory, "bash.exe"))
        out.append(os.path.join(directory, "usr", "bin", "bash.exe"))
    roots = [os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"),
             os.environ.get("ProgramFiles(x86)"), r"C:\Program Files", r"C:\Program Files (x86)"]
    local = os.environ.get("LOCALAPPDATA")
    if local:
        roots.append(os.path.join(local, "Programs"))
    for root in roots:
        if root:
            out.append(os.path.join(root, "Git", "bin", "bash.exe"))
    git = shutil.which("git")
    if git:
        # <root>/cmd/git.exe and <root>/bin/git.exe are both shipped; bash is always in <root>/bin.
        root = os.path.dirname(os.path.dirname(os.path.abspath(git)))
        out.append(os.path.join(root, "bin", "bash.exe"))
        out.append(os.path.join(root, "usr", "bin", "bash.exe"))
    return out


def _resolve_bash():
    r"""The interpreter that runs the entry points, which is not always the `bash` in PATH.

    On Git for Windows, PATH resolves `bash` to the WSL launcher (C:\Windows\system32) ahead of
    the Git Bash that actually implements the POSIX shell these scripts are written in. That stub exits 1 with
    "Windows Subsystem for Linux has no installed distributions." and every dynamic check fails for a reason
    that has nothing to do with the contract. Probe by running the candidate, the same way b3_python probes
    python3 (see scripts/lib/b3_common.sh), and let B3_BASH override it. The probe also has to open this file by
    the path the checks pass: once WSL has a distribution its bash answers `echo ok` too, but it reads a
    C:\... argument as C:Users... and every script check then fails with "No such file or directory".
    """
    candidates = _bash_candidates()
    seen = set()
    for candidate in candidates:
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        try:
            proc = subprocess.run([candidate, "-c", '[ -f "$1" ] && echo ok', "bash", str(Path(__file__).resolve())],
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, encoding="utf-8",
                                  errors="replace", timeout=60)
        except (OSError, subprocess.SubprocessError):
            continue
        if proc.returncode == 0 and proc.stdout.strip() == "ok":
            return candidate
    raise Skip("no working bash interpreter found (set B3_BASH=/path/to/bash)")


_BASH = []


def BASH():
    """Memoised _resolve_bash(): one probe per run, and the failure is reported once as a SKIP."""
    if not _BASH:
        _BASH.append(_resolve_bash())
    return _BASH[0]


def run(name, fn):
    try:
        fn()
    except Skip as exc:
        RESULTS.append(("SKIP", name, str(exc)))
    except AssertionError as exc:
        RESULTS.append(("FAIL", name, str(exc) or "assertion failed"))
    except Exception as exc:  # a broken check is a failure, not a crash
        RESULTS.append(("FAIL", name, "%s: %s" % (type(exc).__name__, exc)))
    else:
        RESULTS.append(("PASS", name, ""))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_text(rel_path):
    return (ROOT / rel_path).read_text(encoding="utf-8", errors="replace")


def code_lines(text):
    """Drop full-line comments: prose may name a flag, code may not. Line numbers stay the file's own."""
    return [(lineno, line) for lineno, line in enumerate(text.splitlines(), 1)
            if not line.lstrip().startswith("#")]


def shell_scripts():
    return sorted(SCRIPTS.rglob("*.sh"))


def sources_lib(rel_path):
    code = "\n".join(line for _, line in code_lines(read_text(rel_path)))
    return re.search(r"(?:^|\n)\s*(?:source|\.)\s+\S*b3_common\.sh", code) is not None


def supports_dry_run(rel_path):
    text = read_text(rel_path)
    return ("B3_DRY_RUN" in text) or ("b3_dry_run" in text)


def game_alive():
    """Never run a dry-run check while a real game is up: a cleanup trap must not be able to signal it."""
    try:
        proc = subprocess.run(["pgrep", "-x", "bayonetta3"], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, encoding="utf-8", errors="replace")
    except OSError:
        return False
    return proc.returncode == 0 and bool(proc.stdout.strip())


def clean_env(extra=None):
    env = {key: value for key, value in os.environ.items() if not key.startswith("NX_")}
    env.update({"B3_DRY_RUN": "1", "B3_ROOT": str(ROOT)})
    if extra:
        env.update(extra)
    return env


# --- static checks -------------------------------------------------------------------------------
def check_driver_contract():
    """The driver is invoked only by scripts/lib/b3_common.sh; everyone else uses its interface.

    A spelling of --game-log/--a-only-until/--take-yes/--movie-* is allowed only in a script that actually
    calls b3_driver_args/b3_start_autodrive, where those names are that function's interface (and the argv
    it returns is the library's). What must never happen is a script building tools/b3_autodrive.py's argv
    itself: that is the drift that left the movie branch dead on one host.
    """
    offenders = []
    for path in shell_scripts():
        rel = path.relative_to(ROOT).as_posix()
        if rel == "scripts/lib/b3_common.sh":
            continue  # the single source of truth spells the driver argv out; everyone else calls it
        lines = code_lines(path.read_text(encoding="utf-8", errors="replace"))
        code = "\n".join(line for _, line in lines)
        if "b3_autodrive.py" in code:
            offenders.append("%s invokes tools/b3_autodrive.py directly (call b3_driver_args instead)" % rel)
            continue
        uses_contract = re.search(r"\bb3_driver_args\b|\bb3_start_autodrive\b", code) is not None
        for lineno, line in lines:
            for flag in FORBIDDEN_DRIVER_FLAGS:
                if flag in line and not uses_contract:
                    offenders.append("%s:%d spells driver flag %s without using b3_driver_args" %
                                     (rel, lineno, flag))
    require(not offenders, "the autodrive invocation must come from b3_driver_args:\n  " +
            "\n  ".join(offenders))


def check_entry_points_source_lib():
    problems, pending = [], []
    for rel in ENTRY_POINTS:
        if not (ROOT / rel).is_file():
            pending.append(rel)
            continue
        if not sources_lib(rel):
            problems.append("%s does not source scripts/lib/b3_common.sh" % rel)
    if problems:
        raise AssertionError("entry points must share one contract:\n  " + "\n  ".join(problems))
    if pending:
        raise Skip("not present yet: " + ", ".join(pending))


def check_bash32_compatible():
    missing = [rel for rel in MACOS_SHARED if not (ROOT / rel).is_file()]
    if missing:
        raise Skip("not present yet: " + ", ".join(missing))
    offenders = []
    for rel in MACOS_SHARED:
        for lineno, line in code_lines(read_text(rel)):
            for pattern, label in BASH4_PATTERNS:
                if pattern.search(line):
                    offenders.append("%s:%d uses %s" % (rel, lineno, label))
    require(not offenders, "macOS /bin/bash is 3.2; bash-4 syntax breaks the mac host:\n  " +
            "\n  ".join(offenders))


# --- dynamic checks (B3_DRY_RUN=1) ---------------------------------------------------------------
def contract_flags():
    proc = subprocess.run([BASH(), "-c", 'source "$1" && b3_driver_contract_flags', "bash", str(LIB)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", errors="replace", timeout=60)
    require(proc.returncode == 0, "cannot read b3_driver_contract_flags: " + proc.stderr.strip())
    flags = [line for line in proc.stdout.split() if line.startswith("--")]
    require(flags, "b3_driver_contract_flags printed no flags")
    return flags


def dry_run(rel, platform, extra_env=None, arguments=("dry-contract", "60")):
    env = clean_env(extra_env)
    env["B3_PLATFORM"] = platform
    proc = subprocess.run([BASH(), str(ROOT / rel)] + list(arguments), cwd=str(ROOT), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", errors="replace", timeout=180)
    return proc


def driver_argv(stdout, rel):
    lines = [line for line in stdout.splitlines() if line.startswith("B3_DRY_RUN: ")]
    require(lines, "%s printed no B3_DRY_RUN: lines (dry-run path missing or silent)" % rel)
    line = next((item for item in lines if "b3_autodrive.py" in item), None)
    require(line is not None, "%s dry-run plan has no autodrive command line" % rel)
    return line[len("B3_DRY_RUN: "):].split()


def check_dynamic_entry_points():
    """Both hosts must print the same contract flags for the same request."""
    rels = ("scripts/b3_experiment.sh", "scripts/b3_mac_run.sh")
    pending = [rel for rel in rels if not (ROOT / rel).is_file()]
    if pending:
        raise Skip("not present yet: " + ", ".join(pending))
    no_dry_run = [rel for rel in rels if not supports_dry_run(rel)]
    if no_dry_run:
        regressed = [rel for rel in no_dry_run if sources_lib(rel)]
        if regressed:
            raise AssertionError("entry point sources b3_common.sh but has no B3_DRY_RUN path: " +
                                 ", ".join(regressed))
        raise Skip("B3_DRY_RUN not implemented yet in " + ", ".join(no_dry_run))
    if game_alive():
        raise Skip("a bayonetta3 process is alive; refusing to run entry points that could signal it")

    tmp = tempfile.mkdtemp(prefix="b3-harness-")
    fake_exe = os.path.join(tmp, "bayonetta3")
    with open(fake_exe, "w", encoding="utf-8") as handle:
        handle.write("#!/bin/sh\nexit 0\n")
    os.chmod(fake_exe, 0o755)
    data_root = os.path.join(tmp, "data")
    os.makedirs(data_root)
    logs = os.path.join(tmp, "logs")
    shared = {"EXE": fake_exe, "DATA_ROOT": data_root, "LOGS": logs, "PRE": "0", "POST": "0"}

    mac = dry_run("scripts/b3_mac_run.sh", "b3-macos",
                  dict(shared, AUTODRIVE="1"))
    linux = dry_run("scripts/b3_experiment.sh", "b3-linux", shared)
    for rel, proc in (("scripts/b3_mac_run.sh", mac), ("scripts/b3_experiment.sh", linux)):
        require(proc.returncode == 0,
                "%s exited %d in dry-run\nstdout:\n%s\nstderr:\n%s"
                % (rel, proc.returncode, proc.stdout, proc.stderr))

    flags = contract_flags()
    plans = {}
    for rel, proc in (("scripts/b3_mac_run.sh", mac), ("scripts/b3_experiment.sh", linux)):
        argv = driver_argv(proc.stdout, rel)
        present = set(argv)
        missing = [flag for flag in flags if flag not in present]
        require(not missing, "%s driver argv is missing contract flags %s\nargv: %s"
                % (rel, missing, " ".join(argv)))
        index = argv.index("--game-log")
        require(index + 1 < len(argv) and not argv[index + 1].startswith("--"),
                "%s passes --game-log without a file\nargv: %s" % (rel, " ".join(argv)))
        require("dry-contract" in argv[index + 1] or "game.log" in argv[index + 1],
                "%s --game-log points at %r instead of this run's game log"
                % (rel, argv[index + 1]))
        plans[rel] = {flag for flag in argv if flag.startswith("--")}
    mac_flags = plans["scripts/b3_mac_run.sh"]
    linux_flags = plans["scripts/b3_experiment.sh"]
    require(mac_flags == linux_flags,
            "the two hosts pass different driver flags:\n  only mac: %s\n  only linux: %s"
            % (sorted(mac_flags - linux_flags), sorted(linux_flags - mac_flags)))


def check_accept_dry_run():
    rel = "scripts/b3_accept.sh"
    if not (ROOT / rel).is_file():
        raise Skip("%s does not exist yet (entrypoint task in flight)" % rel)
    if game_alive():
        raise Skip("a bayonetta3 process is alive; refusing to run the acceptance entry point")
    proc = subprocess.run([BASH(), str(ROOT / rel), "--profile", "movie", "--dry-run",
                           "--platform", "b3-macos"],
                          cwd=str(ROOT), env=clean_env(), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, encoding="utf-8", errors="replace", timeout=180)
    out = proc.stdout + proc.stderr
    require(proc.returncode == 0, "%s --dry-run exited %d\nstdout:\n%s\nstderr:\n%s"
            % (rel, proc.returncode, proc.stdout, proc.stderr))
    markers = ("luma plane fill", "attached movie system", "unresolved", "per frame over 60")
    require(any(marker in out for marker in markers),
            "%s --dry-run does not print the markers it would assert" % rel)
    require("NX_" in out or "strip" in out.lower(),
            "%s --dry-run does not report the stripped diagnostic variables" % rel)


def movie_log_fixture(delivered_fills):
    """A game log shaped like a real movie run: 12 frame windows, 5 progress lines, one closing count.

    `delivered_fills` is the number in the closing `movie: stopping <file>: <N> luma + ... chroma plane
    fills` line. The log prints a `luma plane fill #N` progress line only every 300 frames (a perfect
    1457-frame movie prints five of them), so the closing line is the only honest count. None drops the
    closing line: that log must fail even though it has progress lines.
    """
    lines = [
        "bayonetta3: boot",
        "nvn/vk: linked: 596 modules, 0 unresolved",
    ]
    lines += ["nvn/vk: per frame over 60: %.1f fps, 1.0 submits, replay 3.50 ms, upload 38.44 MB"
              % (54.0 + index * 0.5) for index in range(12)]
    lines.append("movie-vt: attached movie system")
    lines += ["movie: Contents:/movie/ev0005.webm luma plane fill #%d (out 1600x900)" % n
              for n in range(300, 1800, 300)]  # 300..1500: five progress lines, as the real log prints
    if delivered_fills is not None:
        lines.append("movie: stopping Contents:/movie/ev0005.webm: %d luma + %d chroma plane fills"
                     % (delivered_fills, delivered_fills))
    return "\n".join(lines) + "\n"


def accept_from_log(log_path, extra=()):
    # --from-log replays a log and launches nothing, so no B3_DRY_RUN: that switch would short-circuit the
    # whole assertion path and make the test pass without judging anything.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("NX_") and key != "B3_DRY_RUN"}
    env["B3_ROOT"] = str(ROOT)
    return subprocess.run([BASH(), str(ROOT / "scripts/b3_accept.sh"), "--profile", "movie",
                           "--from-log", str(log_path), "--platform", "b3-linux", *extra],
                          cwd=str(ROOT), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          encoding="utf-8", errors="replace", timeout=180)


def check_accept_from_log():
    """The movie criterion is the delivered fill count, never the number of log lines.

    Regression this guards: `movie:` progress lines are throttled (one per 300 frames), so counting them
    marked a perfect 1458-frame run as FAIL. The good fixture therefore has five progress lines and a
    1458-frame closing line: only a criterion that reads the closing count can pass it.
    """
    rel = "scripts/b3_accept.sh"
    if not (ROOT / rel).is_file():
        raise Skip("%s does not exist yet" % rel)
    if "--from-log" not in read_text(rel):
        raise Skip("--from-log is not implemented yet in %s" % rel)
    with tempfile.TemporaryDirectory(prefix="b3-accept-log-") as tmp:
        good = Path(tmp) / "movie-good-game.log"
        good.write_text(movie_log_fixture(1458), encoding="utf-8")
        bad = Path(tmp) / "movie-bad-game.log"
        bad.write_text(movie_log_fixture(None), encoding="utf-8")

        good_run = accept_from_log(good)
        require(good_run.returncode == 0,
                "a movie run that delivered 1458 luma fills must PASS --from-log "
                "(five progress lines is not the count)\nstdout:\n%s\nstderr:\n%s"
                % (good_run.stdout, good_run.stderr))
        require("1458" in good_run.stdout and "max movie luma fills" in good_run.stdout,
                "the PASS must be justified by the delivered fill count\nstdout:\n" + good_run.stdout)

        bad_run = accept_from_log(bad)
        require(bad_run.returncode == 1,
                "a log with progress lines but no closing fill count must FAIL --from-log\n"
                "stdout:\n%s\nstderr:\n%s" % (bad_run.stdout, bad_run.stderr))
        require("max movie luma fills" in bad_run.stdout,
                "the failure must name the fill criterion\nstdout:\n" + bad_run.stdout)

        # A replay has no run, commit or environment: recording it would be a false acceptance claim.
        recorded = accept_from_log(good, extra=("--record",))
        require(recorded.returncode == 2,
                "--from-log --record must be refused (exit 2), not written\nstdout:\n%s\nstderr:\n%s"
                % (recorded.stdout, recorded.stderr))


def check_accept_movie_save_note():
    """A `movie` FAIL caused by the save state must say so, not look like a broken movie repair.

    The trap found on 2026-10-26 (docs/SESSION-2026-10-26-MAC.md §3b): a data root whose save is already
    past the intro drives straight into the Prologue scene and never opens Contents:/movie/, so
    `attached movie system` and the fill count are 0 while the platform is fine. Two identical-looking
    FAILs sent that session chasing the runtime twice. The runner now distinguishes them on stderr.
    """
    rel = "scripts/b3_accept.sh"
    if not (ROOT / rel).is_file():
        raise Skip("%s does not exist yet" % rel)
    with tempfile.TemporaryDirectory(prefix="b3-accept-save-") as tmp:
        progressed = Path(tmp) / "movie-progressed-game.log"
        progressed.write_text(
            "linked: 596 modules, 0 unresolved\n"
            "nvn/vk: per frame over 60: 28.0 fps, 1.0 submits, replay 4.00 ms, upload 38.30 MB\n"
            "fs: slow read Contents:/effect_000.pkz: 131072 bytes at 0 took 1 ms\n",
            encoding="utf-8")
        run_progressed = accept_from_log(progressed)
        require("never opened a movie file" in run_progressed.stderr,
                "a movie run that opened no movie file must say the save state is the cause\n"
                "stdout:\n%s\nstderr:\n%s" % (run_progressed.stdout, run_progressed.stderr))

        # The note must stay quiet when a movie really was opened: a hint that fires on good runs is noise
        # the next agent learns to ignore.
        watched = Path(tmp) / "movie-watched-game.log"
        watched.write_text(movie_log_fixture(1458), encoding="utf-8")
        run_watched = accept_from_log(watched)
        require("never opened a movie file" not in run_watched.stderr,
                "the save-state note must not fire when the run opened a movie\nstderr:\n"
                + run_watched.stderr)

        # And it must be the movie profile's note only: boot/title never play a movie by design.
        boot = accept_from_log_replayed_profile(watched, "boot")
        require("never opened a movie file" not in boot.stderr,
                "the save-state note must not fire for profiles that do not expect a movie\nstderr:\n"
                + boot.stderr)


def accept_from_log_replayed_profile(log_path, profile):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("NX_") and key != "B3_DRY_RUN"}
    env["B3_ROOT"] = str(ROOT)
    return subprocess.run([BASH(), str(ROOT / "scripts/b3_accept.sh"), "--profile", profile,
                           "--from-log", str(log_path), "--platform", "b3-linux"],
                          cwd=str(ROOT), env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          encoding="utf-8", errors="replace", timeout=180)


# --- acceptance gate in a scratch repository ------------------------------------------------------
def git(repo, *args):
    proc = subprocess.run(["git", "-C", str(repo)] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, encoding="utf-8", errors="replace")
    require(proc.returncode == 0, "git %s failed: %s" % (" ".join(args), proc.stderr.strip()))
    return proc.stdout.strip()


def git_commit(repo, name, text, message):
    (repo / name).parent.mkdir(parents=True, exist_ok=True)
    (repo / name).write_text(text, encoding="utf-8")
    git(repo, "add", name)
    git(repo, "-c", "user.name=b3-test", "-c", "user.email=b3-test@example.invalid",
        "commit", "-q", "-m", message)
    return git(repo, "rev-parse", "HEAD")


def write_record(repo, platform, profile, commit, pass_value=True, clean_env=True):
    path = repo / "docs" / "acceptance" / (platform + ".json")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({
        "platform": platform,
        "profiles": {profile: {
            "commit": commit, "date": "2026-01-02T03:04:05+00:00", "pass": pass_value,
            "clean_env": clean_env, "wall_seconds": 60, "fps_median": 55.0, "fills": 1200,
            "fatal": 0, "crash": 0,
            "evidence": {"game_log": "logs/x-game.log", "summary": "logs/x-summary.txt"},
            "note": "scratch repository record",
        }},
    }, indent=2) + "\n", encoding="utf-8")


def acceptance(repo, *args):
    return subprocess.run([PY, str(TOOL), "--repo", str(repo)] + list(args), cwd=str(ROOT),
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", errors="replace", timeout=120)


def check_acceptance_gate():
    """The gate must say FAIL for stale/missing/false/unknown records and PASS only at HEAD."""
    with tempfile.TemporaryDirectory(prefix="b3-accept-gate-") as tmp:
        repo = Path(tmp)
        init = subprocess.run(["git", "-C", str(repo), "init", "-q", "-b", "main"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8", errors="replace")
        if init.returncode != 0:  # git < 2.28 has no init -b
            git(repo, "init", "-q")
        base = git(repo, "symbolic-ref", "--short", "HEAD")  # valid before the first commit
        old = git_commit(repo, "a.txt", "one\n", "first")
        head = git_commit(repo, "a.txt", "two\n", "second")
        require(old != head, "scratch repository needs two commits")

        # 1. no record at all: the honest "nothing was verified" failure.
        write_record(repo, "b3-macos", "movie", old)
        proc = acceptance(repo, "--check", "--platforms", "b3-macos")
        require(proc.returncode == 1, "a record one commit behind HEAD must fail --check with lag 0")
        require("behind HEAD by 1 commit" in proc.stdout,
                "the failure must name the lag\nstdout:\n" + proc.stdout)

        # 2. an allowed lag passes.
        proc = acceptance(repo, "--check", "--max-lag", "1", "--platforms", "b3-macos")
        require(proc.returncode == 0, "--max-lag 1 must accept a record one commit behind\nstdout:\n"
                + proc.stdout + "\nstderr:\n" + proc.stderr)

        # 3. a record at HEAD passes with the default lag of 0.
        write_record(repo, "b3-macos", "movie", head)
        proc = acceptance(repo, "--check", "--platforms", "b3-macos")
        require(proc.returncode == 0, "a HEAD record must pass --check\nstdout:\n" + proc.stdout)
        payload = json.loads(acceptance(repo, "--json", "--check", "--platforms", "b3-macos").stdout)
        require(payload["ok"] is True and payload["problems"] == [], "json output must agree with the gate")

        # 4. pass=false fails even at HEAD.
        write_record(repo, "b3-macos", "movie", head, pass_value=False)
        proc = acceptance(repo, "--check", "--platforms", "b3-macos")
        require(proc.returncode == 1, "a PASS=false record must fail --check")
        require("pass=false" in proc.stdout, "the failure must name pass=false\n" + proc.stdout)

        # 5. a rewritten-away commit is reported as unknown, not a crash.
        write_record(repo, "b3-macos", "movie", "0123456789abcdef0123456789abcdef01234567")
        proc = acceptance(repo, "--check", "--platforms", "b3-macos")
        require(proc.returncode == 1 and "does not exist" in proc.stdout,
                "a vanished commit must fail as unknown\nstdout:\n" + proc.stdout + "\nstderr:\n" + proc.stderr)
        require("Traceback" not in proc.stderr, "the tool must not crash on an unknown commit")

        # 6. a commit on an unmerged branch is not an ancestor: also a failure, with its own wording.
        git(repo, "checkout", "-q", "-b", "side")
        side = git_commit(repo, "b.txt", "side\n", "side commit")
        git(repo, "checkout", "-q", base)
        write_record(repo, "b3-macos", "movie", side)
        proc = acceptance(repo, "--check", "--platforms", "b3-macos")
        require(proc.returncode == 1 and "not an ancestor" in proc.stdout,
                "a branch-only commit must fail as not-an-ancestor\nstdout:\n" + proc.stdout)

        # 7. a missing record file fails with a message that says what to do.
        (repo / "docs" / "acceptance" / "b3-macos.json").unlink()
        proc = acceptance(repo, "--check", "--platforms", "b3-macos")
        require(proc.returncode == 1 and "no acceptance record" in proc.stdout,
                "a missing record must fail\nstdout:\n" + proc.stdout)


def main():
    checks = (
        ("static: the autodrive invocation comes from b3_driver_args", check_driver_contract),
        ("static: entry points source scripts/lib/b3_common.sh", check_entry_points_source_lib),
        ("static: macOS-shared scripts stay bash 3.2 compatible", check_bash32_compatible),
        ("dynamic: both hosts print the same driver flags", check_dynamic_entry_points),
        ("dynamic: scripts/b3_accept.sh --dry-run reports markers and stripped env", check_accept_dry_run),
        ("dynamic: b3_accept.sh --from-log judges delivered fills, not log lines", check_accept_from_log),
        ("dynamic: a movie FAIL from a progressed save names its own cause", check_accept_movie_save_note),
        ("dynamic: acceptance_status.py --check gate in a scratch repository", check_acceptance_gate),
    )
    for name, fn in checks:
        run(name, fn)
    for status, name, detail in RESULTS:
        if status == "PASS":
            print("PASS: %s" % name)
        elif status == "SKIP":
            print("SKIP: %s (%s)" % (name, detail))
        else:
            print("FAIL: %s\n    %s" % (name, detail.replace("\n", "\n    ")))
    passed = sum(1 for status, _, _ in RESULTS if status == "PASS")
    skipped = sum(1 for status, _, _ in RESULTS if status == "SKIP")
    failed = sum(1 for status, _, _ in RESULTS if status == "FAIL")
    print("\n%d passed, %d skipped, %d failed" % (passed, skipped, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
