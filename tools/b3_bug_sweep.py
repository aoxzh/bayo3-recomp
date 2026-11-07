#!/usr/bin/env python3
"""Sweeps a runtime log for every defect signal the runtime can emit, and ranks them.

The runtime already logs each thing it cannot do (a texture it could not make, a shader that
failed, a dispatch it dropped, a handle it did not know). A long automated run produces
thousands of lines, so defects are easy to miss by eye and easy to re-discover by hand.

This groups those lines into named findings with counts, first/last occurrence and a sample,
then prints the findings worst-first. Frames that are mostly black are reported too, because a
run can be free of log errors and still render nothing.

usage:
  b3_bug_sweep.py <log> [<log> ...] [--frames <frames dir>] [--json <out>] [--min-count N]
"""
import argparse
import glob
import json
import os
import re
import sys

# (id, severity, regex, one-line description). Severity: 3 = output is wrong, 2 = content lost,
# 1 = degraded/slow, 0 = informational. Ordered worst-first in the report.
SIGNALS = [
    ("fatal", 3, re.compile(r"^FATAL: (.*)$"), "the runtime stopped"),
    ("submit-lost", 3, re.compile(r"a part of a submit could not begin: it is lost"),
     "a submit was dropped: everything it drew is missing"),
    ("dispatches-lost", 3, re.compile(r"split submit's dispatches was not queued"),
     "a split submit lost its dispatches: compute results missing"),
    ("guest-write-lost", 3, re.compile(r"guest write into a compute-written range \((\d+) bytes\): lost"),
     "a CPU write over GPU-written data was lost"),
    ("pipeline-failed", 3, re.compile(r"pipeline for (\S+) \+ (\S+) failed"), "a graphics pipeline failed"),
    ("compute-pipeline-failed", 3, re.compile(r"compute pipeline for (\S+) failed"), "a compute pipeline failed"),
    ("mask-clear-failed", 3, re.compile(r"mask clear pipeline \(slot (\d+) mask (\S+)\) failed"),
     "a masked clear fell back"),
    ("clipz-failed", 3, re.compile(r"shader (\S+): required clip z patch failed"), "clip-space z was not patched"),
    ("format-unsupported", 3, re.compile(r"format (\S+) \(Vulkan (\d+)\) (\d+x\d+x\d+), .*: not supported by this"),
     "a texture format could not be made"),
    ("format-usage", 2, re.compile(r"format (\S+) \(Vulkan (\d+)\): usage (\S+) not supported, made with (\S+)"),
     "a format was made with reduced usage"),
    ("untile-failed", 3, re.compile(r"texture (\d+x\d+) format (\S+): could not untile"), "a texture was not untiled"),
    ("bc-pitch-linear", 2, re.compile(r"pitch-linear BC texture \(format (\S+)\): not decoded, cleared"),
     "a BC texture was cleared instead of decoded"),
    ("sampled-dummy", 3, re.compile(r"texture (\S+) \((\d+x\d+ x\d+), view type (\d+)\) sampled as view type (\d+): dummy"),
     "a texture was replaced by the dummy"),
    ("dummy-sampled", 2, re.compile(r"dummy sampled: (\d+) with no image, (\d+) with an incompatible view"),
     "draws sampled the dummy image"),
    ("unknown-handle", 2, re.compile(r"called handle (\S+) is unknown \((\d+) calls"), "a guest call named an unknown object"),
    ("submitted-unknown", 2, re.compile(r"submitted handle (\S+) is unknown"), "a submitted command named an unknown object"),
    ("dispatch-skipped", 2, re.compile(r"dispatch skipped dims (\d+),(\d+),(\d+) cs (\S+)"), "a compute dispatch was dropped"),
    ("dispatch-clamped", 1, re.compile(r"dispatch dims (\d+),(\d+),(\d+) clamped"), "a dispatch dimension was clamped"),
    ("dispatch-noprog", 2, re.compile(r"dispatch with no compute program bound"), "a dispatch had no program"),
    ("copy-not-replayed", 2, re.compile(r"copy (\S+) -> (\S+) not replayed"), "a texture copy was skipped"),
    ("no-results-tsv", 2, re.compile(r"no (\S+)results\.tsv: storage buffers"), "storage buffers cannot be bound"),
    ("stream-divisor", 1, re.compile(r"stream (\d+) divisor (\d+) cannot be expressed"), "an instance divisor fell back"),
    ("extensions-missing", 0, re.compile(r"extensions missing: (.*)$"), "optional extensions absent"),
    ("affinity-failed", 1, re.compile(r"sched_setaffinity to role (\d+)'s cores failed (\d+) times"), "core affinity failed"),
    ("shader-inputs-unwritten", 3, re.compile(r"shader (\S+): inputs (\S+) not written by the vertex stage"),
     "a fragment input has no vertex output"),
    ("shader-draws-skipped", 2, re.compile(r"(\d+) draws skipped"), "draws skipped by the pipeline worker"),
    ("swapchain-remade", 1, re.compile(r"present pass failed: swapchain made again"), "the present pass failed"),
    ("vk-call-failed", 3, re.compile(r"nvn/vk: (vk\w+) failed \((\d+)\)"), "a Vulkan call failed"),
    ("missing-import", 2, re.compile(r"missing import called: (\S+)"), "a guest import is unimplemented"),
    ("svc-unknown", 1, re.compile(r"svcGetInfo: unknown type (\d+) sub (\d+)"), "an svcGetInfo selector is unimplemented"),
    ("handle-unknown-svc", 1, re.compile(r"missing import called: (\S+)"), "unimplemented service call"),
    ("fs-open-failed", 2, re.compile(r"OpenFile\((\S+)\) failed to open on the host"), "a file was not found"),
    ("am-unsupported", 2, re.compile(r"CreateTransferMemoryStorage is not supported"), "transfer memory unsupported"),
    ("nro-unsupported", 2, re.compile(r"nn::ro::LoadModule: NRO not supported"), "module loading unsupported"),
    ("counter-type", 0, re.compile(r"first counter report of type (\d+)"), "a GPU counter type appeared"),
    ("not-ready", 1, re.compile(r"textures not ready: (\d+) no image, (\d+) dirty, (\d+) texel writes"),
     "sampled textures were not ready"),
]

FRAME_RE = re.compile(r"^present timing over (\d+): ([\d.]+) fps.*interval (\d+)")


def scan_log(path):
    findings = {}
    order = []
    last_frame = None
    intervals = {}
    line_no = 0
    with open(path, "r", errors="replace") as f:
        for line in f:
            line_no += 1
            line = line.rstrip("\n")
            m = FRAME_RE.match(line)
            if m:
                last_frame = int(m.group(1))
                intervals[int(m.group(3))] = intervals.get(int(m.group(3)), 0) + 1
                continue
            for sid, sev, rx, desc in SIGNALS:
                m = rx.search(line)
                if not m:
                    continue
                f = findings.get(sid)
                if f is None:
                    f = findings[sid] = {
                        "id": sid, "severity": sev, "description": desc, "count": 0,
                        "first_line": line_no, "last_line": line_no, "sample": line[:300],
                        "keys": {},
                    }
                    order.append(sid)
                f["count"] += 1
                f["last_line"] = line_no
                key = m.group(0)[:160]
                f["keys"][key] = f["keys"].get(key, 0) + 1
    return findings, order, last_frame, intervals


def frame_report(directory):
    """Presents frames and the black ones among them."""
    try:
        from PIL import Image
    except ImportError:
        return None
    files = sorted(glob.glob(os.path.join(directory, "[0-9][0-9][0-9][0-9][0-9].ppm")))
    if not files:
        return None
    black = []
    stats = []
    for p in files:
        try:
            im = Image.open(p).convert("L")
        except Exception:
            continue
        lo, hi = im.getextrema()
        stats.append((os.path.basename(p), hi))
        if hi <= 8:
            black.append(os.path.basename(p))
    if not stats:
        return None
    # Runs of consecutive black frames, so one long cutscene does not look like many bugs.
    runs = []
    run_start = None
    prev = None
    for name in black:
        n = int(name[:-4])
        if prev is not None and n - prev <= 2:
            run_start = run_start if run_start is not None else prev
        else:
            if run_start is not None:
                runs.append((run_start, prev))
            run_start = n
        prev = n
    if run_start is not None:
        runs.append((run_start, prev))
    return {"frames": len(stats), "black": len(black), "black_pct": 100.0 * len(black) / len(stats), "runs": runs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--frames", help="frames directory of the same run (black-frame report)")
    ap.add_argument("--json")
    ap.add_argument("--min-count", type=int, default=1)
    a = ap.parse_args()

    merged = {}
    per_log = {}
    for path in a.logs:
        findings, order, last_frame, intervals = scan_log(path)
        per_log[path] = {"last_frame": last_frame, "intervals": intervals}
        for sid, f in findings.items():
            g = merged.get(sid)
            if g is None:
                merged[sid] = dict(f)
                g = merged[sid]
                g["logs"] = {path: f["count"]}
            else:
                g["count"] += f["count"]
                g["logs"][path] = f["count"]
                if f["count"] > 0:
                    g["sample"] = f["sample"]

    ranked = sorted(merged.values(), key=lambda f: (-f["severity"], -f["count"]))
    print(f"=== bug sweep over {len(a.logs)} log(s) ===")
    for path in a.logs:
        info = per_log[path]
        iv = ", ".join(f"interval {k}: {v}" for k, v in sorted(info["intervals"].items()))
        print(f"  {path}: last frame {info['last_frame']}, {iv or 'no timing lines'}")

    shown = [f for f in ranked if f["count"] >= a.min_count]
    if not shown:
        print("\nno defects matched")
    for f in shown:
        print(f"\n[{f['severity']}] {f['id']}: {f['description']}")
        print(f"    {f['count']} occurrence(s), lines {f['first_line']}..{f['last_line']}")
        if len(f["logs"]) > 1:
            print("    per log: " + ", ".join(f"{os.path.basename(k)}={v}" for k, v in f["logs"].items()))
        for key, n in sorted(f["keys"].items(), key=lambda kv: -kv[1])[:4]:
            print(f"    x{n:<6d} {key}")
        if f["severity"] >= 2:
            print(f"    sample: {f['sample']}")

    frames = frame_report(a.frames) if a.frames else None
    if frames:
        print(f"\n=== frames: {frames['frames']} dumped, {frames['black']} black "
              f"({frames['black_pct']:.1f}%) ===")
        for lo, hi in frames["runs"][:20]:
            print(f"    black run {lo}..{hi}" + (f" ({hi - lo + 1} frames)" if hi > lo else ""))

    if a.json:
        with open(a.json, "w") as out:
            json.dump({"findings": ranked, "logs": per_log, "frames": frames}, out, indent=2)
        print(f"\nwrote {a.json}")

    worst = [f for f in ranked if f["severity"] >= 3]
    return 1 if worst else 0


if __name__ == "__main__":
    sys.exit(main())
