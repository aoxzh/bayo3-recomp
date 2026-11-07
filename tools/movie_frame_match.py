#!/usr/bin/env python3
"""Match dumped game frames against the source .webm a cutscene was decoded from.

Acceptance is not "one frame looked like the movie": a cutscene that plays must show the movie's
picture *in step with its own timeline*, in colour, for a stretch of the movie. So this tool:

  1. decodes the reference movie to a small RGB thumbnail stream (default 10 Hz, 160x90) -- scaling
     and encoder noise stop dominating, and colour stays available;
  2. reduces every dumped frame (<data root>/frames/NNNNN.ppm, whatever size) to the same grid,
     keeping luma and per-channel means;
  3. scores every dump against every reference thumbnail (mean absolute error, luma and colour) and
     takes the best offset -- a dump is taken at an arbitrary point in the movie, so the offset is
     searched, not assumed;
  4. looks for a *timeline*: the longest run of consecutive dumps whose matched reference times
     strictly increase and that spans at least --min-span seconds of the movie;
  5. requires that run to contain non-black evidence in its early, middle and late part, and that
     saturation (colour) matches the reference -- a greyscale or washed-out picture fails even when
     its luma matches.

It prints a per-frame table (luma MAE, colour MAE, ours/reference luma and saturation, verdict), the
timeline, and PASS/FAIL with reasons. Exit status is 0 only on PASS.

usage:
  python3 tools/movie_frame_match.py --frames <data root>/frames --movie ev0005.webm \
      [--from 3000] [--to 3600] [--hz 10] [--min-run 3] [--min-span 2.0]
"""
import argparse
import os
import subprocess
import sys
import warnings

warnings.filterwarnings("ignore", category=DeprecationWarning)  # PIL's getdata() on modern Pillow

GRID = (160, 90)          # comparison grid for both sides
BLACK_LUMA = 12.0         # a frame whose luma grid averages below this is "black" (fades, opening black)
GRAY_MAE = 12.0           # default per-frame luma acceptance
COLOR_MAE = 26.0          # default per-frame colour acceptance (per-channel mean abs difference)
SAT_TOL = 12.0            # mean saturation difference allowed across the matched run


def read_ppm(path):
    """P6 RGB bytes and (w, h) of a binary PPM."""
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P6":
            raise ValueError(f"{path}: not a binary PPM ({magic!r})")
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = (int(x) for x in line.split())
        maxval = int(f.readline())
        if maxval != 255:
            raise ValueError(f"{path}: maxval {maxval} is not 8-bit")
        data = f.read(w * h * 3)
    if len(data) != w * h * 3:
        raise ValueError(f"{path}: short read ({len(data)} of {w * h * 3})")
    return data, w, h


def thumbnail(data, w, h, grid=GRID):
    """(luma grid, (r, g, b) means, saturation mean) of one frame, letterbox bars removed.

    A cutscene is displayed letterboxed inside the 1600x900 frame while the reference movie is 1600x900 of picture:
    comparing the bars against picture content buries a real match under a large error. Rows that are black at the
    top and bottom of *our* frame (and only rows whose removal leaves at least half the frame) are cropped away, so
    the grid maps picture onto picture exactly as the reference does.
    """
    from PIL import Image

    im = Image.frombytes("RGB", (w, h), data)
    small = im.resize(grid, Image.BOX).convert("L")
    lum_small = list(small.getdata())
    rowmean = [sum(lum_small[y * grid[0]:(y + 1) * grid[0]]) / grid[0] for y in range(grid[1])]
    top, bot = 0, grid[1]
    while top < grid[1] // 3 and rowmean[top] < 6.0:
        top += 1
    while bot > 2 * grid[1] // 3 and rowmean[bot - 1] < 6.0:
        bot -= 1
    if top and bot < grid[1] and (bot - top) * 2 >= grid[1]:
        im = im.crop((0, int(top * h / grid[1]), w, int(bot * h / grid[1])))
    im = im.resize(grid, Image.BOX)
    px = list(im.getdata())
    n = len(px)
    lum = bytes(int(0.299 * r + 0.587 * g + 0.114 * b + 0.5) for r, g, b in px)
    r = sum(p[0] for p in px) / n
    g = sum(p[1] for p in px) / n
    b = sum(p[2] for p in px) / n
    sat = sum(max(p) - min(p) for p in px) / n
    return lum, (r, g, b), sat


def reference_thumbs(movie, hz, grid=GRID):
    """[(index, luma grid, (r,g,b), saturation)] of the movie at `hz` frames a second."""
    cmd = ["ffmpeg", "-v", "error", "-i", movie, "-vf",
           f"fps={hz},scale={grid[0]}:{grid[1]}", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"]
    out = subprocess.run(cmd, capture_output=True, check=False)
    if out.returncode != 0:
        raise SystemExit(f"ffmpeg failed on {movie}: {out.stderr.decode(errors='replace').strip()}")
    size = grid[0] * grid[1] * 3
    raw = out.stdout
    refs = []
    for i in range(len(raw) // size):
        refs.append((i, *thumbnail(raw[i * size:(i + 1) * size], grid[0], grid[1], grid)))
    return refs


def mae(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / float(len(a))


def colour_mae(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / 3.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", required=True, help="directory of NNNNN.ppm dumps")
    ap.add_argument("--movie", required=True, help="the .webm the session decoded")
    ap.add_argument("--from", dest="lo", type=int, default=0, help="first dumped frame number to look at")
    ap.add_argument("--to", dest="hi", type=int, default=10 ** 9, help="last dumped frame number")
    ap.add_argument("--every", type=int, default=1, help="only every Nth dumped frame")
    ap.add_argument("--hz", type=float, default=10.0, help="reference sampling rate")
    ap.add_argument("--min-run", type=int, default=3, help="consecutive dumps that must match")
    ap.add_argument("--min-span", type=float, default=2.0, help="seconds of movie the run must span")
    ap.add_argument("--gray", type=float, default=GRAY_MAE, help="per-frame luma MAE allowed")
    ap.add_argument("--colour", type=float, default=COLOR_MAE, help="per-frame colour MAE allowed")
    a = ap.parse_args()

    names = sorted((n for n in os.listdir(a.frames) if n.endswith(".ppm") and n[:-4].isdigit()),
                   key=lambda n: int(n[:-4]))
    picked = [n for n in names if a.lo <= int(n[:-4]) <= a.hi][::a.every]
    if not picked:
        raise SystemExit(f"no dumped frames in {a.frames} between {a.lo} and {a.hi}")
    refs = reference_thumbs(a.movie, a.hz)
    if not refs:
        raise SystemExit(f"{a.movie} decoded to no frames")
    print(f"reference: {os.path.basename(a.movie)} {len(refs)} frames at {a.hz} Hz "
          f"({len(refs) / a.hz:.2f} s); dumps: {len(picked)} of {len(names)} in the window")

    rows = []
    for name in picked:
        try:
            data, w, h = read_ppm(os.path.join(a.frames, name))
        except (ValueError, OSError) as exc:
            print(f"{name}: unreadable ({exc})")
            continue
        lum, rgb, sat = thumbnail(data, w, h)
        best = None
        for ref in refs:
            d = mae(lum, ref[1])
            if best is None or d < best[0]:
                best = (d, ref)
        gmae, ref = best
        cmae = colour_mae(rgb, ref[2])
        luma = sum(lum) / float(len(lum))
        ref_luma = sum(ref[1]) / float(len(ref[1]))
        ok = gmae <= a.gray and cmae <= a.colour
        # A black frame against a black reference frame has MAE 0 but proves nothing about the picture: it is its
        # own verdict and never counts towards the timeline.
        if luma < BLACK_LUMA and ref_luma < BLACK_LUMA:
            verdict = "black"
        elif ok:
            verdict = "match"
        else:
            verdict = "differs"
        rows.append(dict(name=name, n=int(name[:-4]), t=ref[0] / a.hz, gmae=gmae, cmae=cmae, luma=luma,
                         ref_luma=ref_luma, sat=sat, ref_sat=ref[3], verdict=verdict))
        print(f"{name}: ours luma {luma:5.1f} sat {sat:5.1f} | ref t={ref[0] / a.hz:6.2f}s luma {ref_luma:5.1f} "
              f"sat {ref[3]:5.1f} | mae {gmae:5.1f} colour {cmae:5.1f}  {verdict}")

    # Timeline: the longest run of consecutive matched dumps whose reference time strictly increases. Real playback
    # produces exactly that; a coincidental single-frame hit does not.
    best_run, run = [], []
    for row in [r for r in rows if r["verdict"] == "match"]:
        if run and not (row["t"] > run[-1]["t"]):
            run = []
        run.append(row)
        if len(run) > len(best_run):
            best_run = list(run)
    if best_run:
        print(f"\nlongest increasing matched run: {len(best_run)} dump(s), t {best_run[0]['t']:.2f} -> "
              f"{best_run[-1]['t']:.2f} s")
        print("  " + ", ".join(f"{r['name']}@{r['t']:.2f}s" for r in best_run))

    def fail(reason):
        print(f"FAIL: {reason}")
        return 1

    if len(best_run) < a.min_run:
        return fail(f"{len(best_run)} consecutive matched frame(s), need {a.min_run}")
    span = best_run[-1]["t"] - best_run[0]["t"]
    if span < a.min_span:
        return fail(f"matched run spans {span:.2f}s of the movie, need {a.min_span:.2f}s")
    # Non-black evidence in the early / middle / late third: real content, not a fade.
    bounds = [0, max(1, len(best_run) // 3), max(2, 2 * len(best_run) // 3), len(best_run)]
    for label, lo, hi in (("early", bounds[0], bounds[1]), ("middle", bounds[1], bounds[2]),
                          ("late", bounds[2], bounds[3])):
        part = best_run[lo:hi]
        if not part:
            return fail(f"no dumps left for the {label} third of the run")
        if not any(r["luma"] >= BLACK_LUMA and r["ref_luma"] >= BLACK_LUMA for r in part):
            return fail(f"{label} third of the run has no non-black frame "
                        f"(luma {[round(r['luma'], 1) for r in part]})")
    sats = [r["sat"] for r in best_run]
    sat_err = mae(sats, [r["ref_sat"] for r in best_run])
    if sat_err > SAT_TOL:
        return fail(f"saturation differs by {sat_err:.1f} (limit {SAT_TOL}): the picture is not in colour like "
                    f"the movie ({[round(s, 1) for s in sats]} vs {[round(r['ref_sat'], 1) for r in best_run]})")
    print(f"PASS: {len(best_run)} consecutive frames match the source over {span:.2f}s, non-black in the early, "
          f"middle and late part, saturation error {sat_err:.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
