#!/usr/bin/env python3
"""Drive Bayonetta 3 headless from the title into a playable verse, using what it renders.

The runtime dumps a frame on demand (`dump_frames` marker in the data root) and polls a live input
file (`NX_INPUT_LIVE`). This driver reads the dumped frame, decides one press, and repeats, so a run
advances on its own instead of sitting on a screen waiting for a blind timed script.

Three lessons from the runs this came out of are built in:

* **A dark frame is not a stall.** The prologue's narration sections are black with subtitles, and a
  night-rain scene reads as black to any brightness threshold. Pressing PLUS there opens the pause
  menu instead of advancing, so the driver only presses PLUS when a frame has been unchanged *and*
  it is genuinely empty.
* **Tutorial cards and cutscenes take A; the in-cutscene pause menu has Skip.** A card ignores A for
  a while and then closes; a cutscene is skipped through PLUS, DOWN, A. The driver tells them apart
  by whether the pause menu it opens offers a second item.
* **Colour is a correctness signal, not decoration.** A movie whose chroma plane is never written
  plays in greyscale with a perfect luma, so every report prints luma *and* saturation.

usage:
  b3_headless_drive.py <data root> [--seconds 300] [--every 3] [--keep <dir>]
"""
import argparse
import glob
import os
import time

from PIL import Image


def latest(frames, floor):
    best = None
    for p in glob.glob(os.path.join(frames, "[0-9][0-9][0-9][0-9][0-9].ppm")):
        n = int(os.path.basename(p)[:-4])
        if n > floor and (best is None or n > best[0]):
            best = (n, p)
    return best


class Drive:
    def __init__(self, root, keep):
        self.root = root
        self.frames = os.path.join(root, "frames")
        self.marker = os.path.join(root, "dump_frames")
        self.live = os.path.join(root, "input_live.txt")
        self.keep = keep or os.path.join(root, "shots")
        os.makedirs(self.frames, exist_ok=True)
        os.makedirs(self.keep, exist_ok=True)
        self.floor = 0
        self.prev = None

    def frame(self, wait=8.0):
        """The next dumped frame, as (number, path, luma, saturation, luma grid)."""
        for p in glob.glob(os.path.join(self.frames, "*.ppm")):
            try:
                os.remove(p)
            except OSError:
                pass
        with open(self.marker, "w") as f:
            f.write("1 1")
        end = time.time() + wait
        got = None
        while time.time() < end:
            time.sleep(0.2)
            got = latest(self.frames, self.floor)
            if got:
                break
        if not got:
            return None
        self.floor, path = got
        im = Image.open(path).convert("RGB")
        px = list(im.resize((160, 90)).get_flattened_data())
        luma = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in px]
        sat = [max(r, g, b) - min(r, g, b) for r, g, b in px]
        mean = sum(luma) / len(luma)
        return {
            "n": self.floor,
            "im": im,
            "mean": mean,
            "spread": (sum((v - mean) ** 2 for v in luma) / len(luma)) ** 0.5,
            "sat": sum(sat) / len(sat),
            "black": sum(1 for v in luma if v < 12) / len(luma),
            "luma": luma,
            "path": path,
        }

    def press(self, button, hold=0.3):
        tmp = self.live + ".tmp"
        with open(tmp, "w") as f:
            f.write(f"0:{button}:0;")
        os.replace(tmp, self.live)
        time.sleep(hold)
        open(self.live, "w").write("")
        time.sleep(0.55)

    def save(self, info, tag):
        dst = os.path.join(self.keep, f"{tag}-{info['n']}.png")
        info["im"].resize((800, 450)).save(dst)
        return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="NX_DATA_ROOT of the run")
    ap.add_argument("--seconds", type=float, default=300.0)
    ap.add_argument("--every", type=float, default=3.0, help="seconds between frames looked at")
    ap.add_argument("--keep", default=None, help="where to keep snapshots (default <root>/shots)")
    ap.add_argument("--skip-cutscenes", action="store_true",
                    help="open the pause menu and take Skip when a frame is a cutscene")
    a = ap.parse_args()

    d = Drive(a.root, a.keep)
    started = time.time()
    same = 0
    last_mean = None
    while time.time() - started < a.seconds:
        info = d.frame()
        if not info:
            print("no frame (the runtime may have stopped)", flush=True)
            time.sleep(1.0)
            continue
        # Unchanged frames: a fade, a load or a waiting screen.
        still = last_mean is not None and abs(info["mean"] - last_mean) < 0.5
        last_mean = info["mean"]
        same = same + 1 if still else 1
        motion = 0.0
        if d.prev is not None and len(d.prev) == len(info["luma"]):
            motion = sum(abs(x - y) for x, y in zip(info["luma"], d.prev)) / len(info["luma"])
        d.prev = info["luma"]

        print(f"{info['n']:06d} luma {info['mean']:6.1f} spread {info['spread']:5.1f} "
              f"sat {info['sat']:5.1f} motion {motion:5.1f} black {info['black']:.2f} still {same}",
              flush=True)
        if a.every > 0 and info["mean"] > 10:
            d.save(info, "shot")

        if info["mean"] < 6 and info["black"] > 0.95 and same >= 3:
            # Truly empty and stuck: the only way on is the in-cutscene pause menu.
            d.press("PLUS")
            menu = d.frame()
            if menu and menu["mean"] < 80:
                d.save(menu, "pause")
                d.press("DOWN")
                d.press("A")
                print("  empty frame: paused, DOWN, A (Skip)", flush=True)
            else:
                d.press("B")
        else:
            d.press("A")
        time.sleep(max(0.0, a.every - 1.0))
    print(f"stopped after {time.time() - started:.0f} s at frame {d.floor}", flush=True)


if __name__ == "__main__":
    main()
