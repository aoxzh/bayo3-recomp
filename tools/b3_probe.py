#!/usr/bin/env python3
"""Press buttons into a RUNNING headless game and dump a frame after every step.

This is for the places the frame-driven driver cannot solve by itself (the bar hub's world map,
a menu whose selected entry is unknown): a person-looking agent tries candidate inputs, reads the
resulting frames, and turns the winning step into a driver rule.

The game must already be running with NX_INPUT_LIVE pointing at <data root>/input_live.txt and
frame dumps enabled on demand (drop a `dump_frames` file). Every step writes the button to the
live file, releases it, then asks for a frame and prints its luma statistics plus the difference
to the previous step's frame, so "the screen changed" and "nothing happened" are distinguishable.

usage:
  b3_probe.py <data root> "A" "RIGHT:1.0" "wait:2" "A" "PLUS:0.3"
    BUTTON[:seconds]  hold BUTTON (default --hold) then release   (A B X Y L R ZL ZR PLUS MINUS
                      LEFT RIGHT UP DOWN)
    wait:SECONDS      do nothing for SECONDS
    dump              only dump a frame (no input)

Frames are saved as PNG under <data root>/probe/ so they can be looked at.
"""
import argparse
import glob
import os
import sys
import time

BUTTONS = ("A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "RIGHT", "UP", "DOWN")


def write_live(path, button):
    tmp = path + ".probe"
    with open(tmp, "w") as f:
        if button:
            f.write(f"0:{button}:0;")
    os.replace(tmp, path)


def latest(path):
    best = None
    for p in glob.glob(os.path.join(path, "[0-9][0-9][0-9][0-9][0-9].ppm")):
        try:
            n = int(os.path.basename(p)[:-4])
        except ValueError:
            continue
        if best is None or n > best[0]:
            best = (n, p)
    return best


def dump_frame(root, floor, timeout=8.0):
    with open(os.path.join(root, "dump_frames"), "w") as f:
        f.write("1 1")
    end = time.time() + timeout
    got = None
    while time.time() < end:
        time.sleep(0.2)
        got = latest(os.path.join(root, "frames"))
        if got and got[0] > floor:
            return got
    return None


def stats(path):
    from PIL import Image
    im = Image.open(path).convert("RGB").resize((160, 90))
    px = list(im.getdata())
    lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in px]
    n = len(lum)
    mean = sum(lum) / n
    return mean, (sum((v - mean) ** 2 for v in lum) / n) ** 0.5, sum(1 for v in lum if v > 200) / n, lum


def motion(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_root")
    ap.add_argument("steps", nargs="+")
    ap.add_argument("--hold", type=float, default=0.35)
    ap.add_argument("--settle", type=float, default=1.5, help="seconds after a step before the frame")
    ap.add_argument("--probe-dir", default="probe")
    a = ap.parse_args()

    root = a.data_root
    live = os.path.join(root, "input_live.txt")
    out = os.path.join(root, a.probe_dir)
    os.makedirs(out, exist_ok=True)
    # A runtime that was restarted numbers frames from zero again, and frames/ may still hold the
    # previous run's dumps (higher numbers): working from "newest so far" then only accepting a
    # *higher* number would wait forever. The game only writes a frame when asked, so clearing the
    # directory before the first request is safe and makes every number this session's.
    for p_old in glob.glob(os.path.join(root, "frames", "*.ppm")):
        try:
            os.remove(p_old)
        except OSError:
            pass
    for p_old in glob.glob(os.path.join(root, "frames", "*.raw")):
        try:
            os.remove(p_old)
        except OSError:
            pass
    prev = None
    floor = 0
    for i, step in enumerate(a.steps, 1):
        label = step
        hold = a.hold
        if step.startswith("wait:"):
            time.sleep(float(step.split(":", 1)[1]))
            action = None
        elif step == "dump":
            action = None
        else:
            if ":" in step:
                name, secs = step.split(":", 1)
                hold = float(secs)
            else:
                name = step
            if name not in BUTTONS:
                sys.exit(f"unknown step: {step}")
            write_live(live, name)
            time.sleep(hold)
            write_live(live, "")
            action = name
        time.sleep(a.settle)
        got = dump_frame(root, floor)
        if not got:
            print(f"{i:02d} {label:12s} -> no new frame")
            continue
        floor, path = got
        mean, spread, bright, lum = stats(path)
        delta = motion(prev, lum) if prev is not None else 0.0
        prev = lum
        from PIL import Image
        png = os.path.join(out, f"{i:02d}-{label.replace(':', '-')}.png")
        Image.open(path).convert("RGB").save(png)
        print(f"{i:02d} {label:12s} -> frame {floor:05d} mean {mean:6.2f} spread {spread:5.1f} "
              f"bright {bright:.3f} change {delta:6.1f}  {png}")


if __name__ == "__main__":
    sys.exit(main())
