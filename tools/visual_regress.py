#!/usr/bin/env python3
"""Per-scene visual baseline regression for Bayonetta 3.

Captures one golden frame per stable scene from a headless run, then compares a later run's frames
against those goldens with cheap perceptual metrics and fails loudly when a scene changes, loses
colour, goes black or disappears.

    python3 tools/visual_regress.py capture --exe /path/bayonetta3 --data-root /path/data/run --label macos-moltenvk
    python3 tools/visual_regress.py check --frames /path/data/run/frames --label macos-moltenvk
    python3 tools/visual_regress.py bless --from /path/data/run/frames --label macos-moltenvk --note "why"
    python3 tools/visual_regress.py verify --exe /path/bayonetta3 --data-root /path/data/run --label ...
    python3 tools/visual_regress.py --selftest

Goldens live under private/bayonetta3/visual/<label>/ and are never committed (game pixels stay
out of git). One golden set is valid only for its label: another GPU, driver or OS renders the
same frame with different sub-pixel noise and optional FX, so capture its own goldens there.

How a scene is found: by its pixels and by its order in the boot flow, never by an absolute frame
number (load time and frame rate move frame numbers run to run). Stable screens become goldens;
the opening movie, fades and other changing content are recorded as dynamic and are not diffed.
The diff runs at 480x270 and combines luma MAE, Cb/Cr MAE, block SSIM, an 8x5 worst-tile luma MAE,
a saturation ratio and black share. Thresholds come from tools/visual_baseline.json and sit above
the two-run noise floor measured for that label (see docs/REGRESSION.md).

Exit codes: 0 pass (or skipped: no goldens for the label), 1 visual regression, 2 usage/config.
"""
from __future__ import annotations

import argparse
import getpass
import hashlib
import json
import os
import platform
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:  # pragma: no cover - the message is the test
    print("visual_regress: Pillow is required (pip install pillow)", file=sys.stderr)
    raise

VERSION = 1
TOOL = "visual_regress.py"
HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_CONFIG = HERE / "visual_baseline.json"
DEFAULT_GOLDENS_ROOT = REPO / "private" / "bayonetta3" / "visual"
FRAME_EXTS = {".ppm", ".png", ".jpg", ".jpeg"}

DEFAULT_CONFIG_DATA = {
    "version": 1,
    "goldens_root": "private/bayonetta3/visual",
    "check": {
        "width": 480,
        "height": 270,
        "tiles": [8, 5],
        "match": {
            "luma_mae_max": 10.0,
            "chroma_mae_max": 12.0,
            "edge_mae_max": 6.0,
            "ssim_min": 0.55,
            "missing_penalty": 60.0,
        },
        "thresholds": {
            "luma_mae_max": 6.0,
            "chroma_mae_max": 7.0,
            "ssim_min": 0.90,
            "tile_luma_mae_max": 18.0,
            "tile_chroma_mae_max": 24.0,
            "mean_luma_ratio_min": 0.88,
            "mean_luma_ratio_max": 1.12,
            "sat_ratio_min": 0.65,
            "sat_ratio_max": 1.45,
            "black_share_max": 0.60,
            "min_mean_luma": 6.0,
            "max_black_tiles": 0,
        },
    },
    "capture": {
        "seconds": 240.0,
        "every": 1.1,
        "frame_timeout": 10.0,
        "no_frame_timeout": 45.0,
        "boot_timeout": 150.0,
        "title_timeout": 120.0,
        "stable_edge_mae_max": 2.5,
        "stable_chroma_mae_max": 2.0,
        "stable_luma_mae_max": 45.0,
        "dedupe_edge_mae_max": 4.0,
        "dedupe_luma_mae_max": 45.0,
        "min_stable_samples": 3,
        "screen_change_edge": 5.0,
        "screen_change_luma": 60.0,
        "max_dwell_samples": 6,
        "title_dwell_samples": 4,
        "settle": 0.22,
        "max_scenes": 10,
    },
    "scenes": {},
    "labels": {},
}


class VisualError(Exception):
    """A captured run / configuration problem (exit 2), not a visual regression."""


class CaptureEmpty(VisualError):
    """The run produced no usable frames/scene (retriable; still exit 2, never a visual mismatch)."""


class Regression(Exception):
    """A visual regression (exit 1) raised by the selftest helpers."""


# --------------------------------------------------------------------------------------------------
# configuration, platform and small utilities
# --------------------------------------------------------------------------------------------------

def _deep_update(base, extra):
    if not isinstance(base, dict) or not isinstance(extra, dict):
        return extra
    out = dict(base)
    for key, value in extra.items():
        if key in out and isinstance(out[key], dict) and isinstance(value, dict):
            out[key] = _deep_update(out[key], value)
        else:
            out[key] = value
    return out


def load_config(path=None):
    cfg = json.loads(json.dumps(DEFAULT_CONFIG_DATA))
    path = Path(path) if path else DEFAULT_CONFIG
    if path.exists():
        with open(path, "r", encoding="utf-8") as f:
            cfg = _deep_update(cfg, json.load(f))
        cfg["_config_path"] = str(path)
        cfg["_config_sha256"] = sha256_file(path)
    else:
        cfg["_config_path"] = str(path)
        cfg["_config_sha256"] = None
    return cfg


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def default_label():
    env = os.environ.get("NX_VISUAL_LABEL")
    if env:
        return env
    if sys.platform == "darwin":
        return "macos-moltenvk"
    if os.name == "nt":
        return "windows"
    driver = "generic"
    for candidate in ("amdgpu", "i915", "xe", "nvidia", "radeon"):
        if Path("/sys/module/" + candidate).exists():
            driver = candidate
            break
    return "linux-" + driver


def platform_info():
    info = {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "pillow": getattr(Image, "__version__", "?"),
        "hostname": platform.node(),
        "user": getpass.getuser(),
    }
    try:
        info["cpu_count"] = os.cpu_count()
    except Exception:
        pass
    return info


def goldens_root_from(cfg, override=None):
    if override:
        return Path(override).expanduser()
    env = os.environ.get("NX_VISUAL_GOLDENS")
    if env:
        return Path(env).expanduser()
    root = Path(cfg.get("goldens_root", DEFAULT_CONFIG_DATA["goldens_root"]))
    return root if root.is_absolute() else REPO / root


def label_dir_for(label, cfg, goldens_root=None):
    return goldens_root_from(cfg, goldens_root) / label


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def write_goal(root, **fields):
    """Write <NX_DATA_ROOT>/goal for crash reports: UTF-8 key=value lines, well under 2 KiB.

    Written atomically from the capture driver so a crash during a visual run says which scene the
    driver was on (the runtime's crash report picks the file up; NX_GOAL in the environment would
    override it -- we record that in the manifest instead of setting it).
    """
    lines = []
    for key, value in fields.items():
        text = str(value).replace("\n", " ").replace("\r", " ")[:200]
        lines.append(f"{key}={text}")
    content = "\n".join(lines) + "\n"
    if len(content.encode("utf-8")) > 2048:
        content = content[:1800] + "\n"
    path = Path(root) / "goal"
    try:
        tmp = path.with_suffix(".tmp")
        tmp.write_text(content, encoding="utf-8")
        os.replace(tmp, path)
    except OSError:
        pass


def other_game_processes(name="bayonetta3"):
    """PIDs/commands of other game binaries at capture start: two runs on one GPU distort timing."""
    out = []
    try:
        if os.name == "nt":
            text = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True,
                                  text=True, timeout=10).stdout
            for line in text.splitlines():
                if name.lower() in line.lower():
                    out.append(line.strip())
        else:
            text = subprocess.run(["ps", "-axo", "pid=,command="], capture_output=True,
                                  text=True, timeout=10).stdout
            for line in text.splitlines():
                line = line.strip()
                if name in line:
                    out.append(line)
    except (OSError, subprocess.SubprocessError):
        pass
    return out


def _pixels(im):
    """Flat pixel list of a Pillow image, for old and new Pillow."""
    getter = getattr(im, "get_flattened_data", None) or im.getdata
    return list(getter())


def _luma_bytes(im):
    return _pixels(im.convert("L"))


def _mean(values):
    return sum(values) / len(values) if values else 0.0


# --------------------------------------------------------------------------------------------------
# frames and goldens on disk
# --------------------------------------------------------------------------------------------------

def frame_number(path):
    m = re.match(r"(\d+)", Path(path).stem)
    return int(m.group(1)) if m else None


def list_frames(path):
    path = Path(path)
    if path.is_file():
        return [path]
    if not path.exists():
        raise VisualError(f"frames directory does not exist: {path}")
    out = []
    for p in path.iterdir():
        if p.is_file() and p.suffix.lower() in FRAME_EXTS:
            try:
                with Image.open(p) as im:
                    im.size  # a truncated tail frame is not a frame
            except Exception:
                continue
            out.append(p)
    out.sort(key=lambda p: (frame_number(p) is None, frame_number(p) or 0, p.name))
    return out


def open_rgb(path):
    im = Image.open(path)
    im.load()
    return im.convert("RGB")


def load_goldens(label_dir, cfg=None):
    """The golden store of a label, or None when it has not been captured yet."""
    label_dir = Path(label_dir)
    manifest_path = label_dir / "manifest.json"
    if not manifest_path.exists():
        return None
    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    scenes = []
    for meta in manifest.get("scenes", []):
        png = label_dir / meta["file"]
        if not png.exists():
            raise VisualError(f"golden {meta['scene']}: missing {png}")
        digest = sha256_file(png)
        if meta.get("sha256") and digest != meta["sha256"]:
            raise VisualError(f"golden {meta['scene']}: sha256 changed on disk ({meta['sha256']} -> {digest})")
        im = open_rgb(png)
        scenes.append({
            "scene": meta["scene"],
            "file": meta["file"],
            "path": str(png),
            "meta": meta,
            "im": im,
            "feat": analyze_rgb(im),
            "check_feat": _check_feat(im, cfg),
        })
    manifest["_dir"] = str(label_dir)
    return {"manifest": manifest, "scenes": scenes, "label_dir": str(label_dir)}


def write_goldens(label_dir, scenes, manifest):
    """Write PNG goldens + manifest into the local (gitignored) goldens folder."""
    label_dir = Path(label_dir)
    label_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    keep = set()
    for order, scene in enumerate(scenes):
        name = _safe_name(scene["name"])
        meta = dict(scene.get("meta", {}))
        png_name = f"{order:02d}-{name}.png"
        png = label_dir / png_name
        im = scene["im"]
        tmp = label_dir / (png_name + ".tmp")
        im.save(tmp, format="PNG")
        os.replace(tmp, png)
        keep.add(png_name)
        entry = {
            "scene": name,
            "order": order,
            "file": png_name,
            "frame": scene.get("frame"),
            "present": scene.get("present"),
            "resolution": list(im.size),
            "sha256": sha256_file(png),
            "mean_luma": round(scene.get("mean_luma", _mean(_luma_bytes(im))), 3),
            "luma_std": round(scene.get("luma_std", 0.0), 3),
            "saturation": round(scene.get("saturation", 0.0), 3),
            "black_share": round(scene.get("black_share", 0.0), 4),
            "bright_share": round(scene.get("bright_share", 0.0), 4),
            "stable_samples": scene.get("stable_samples"),
            "source_frame": scene.get("source_frame"),
            "captured_at": scene.get("captured_at", iso_now()),
        }
        entry.update(meta)
        entries.append(entry)
    for old in label_dir.glob("*.png"):
        if old.name not in keep:
            old.unlink()
    manifest = dict(manifest)
    manifest["scenes"] = entries
    manifest["tool"] = TOOL
    manifest["version"] = VERSION
    manifest["updated"] = iso_now()
    manifest_path = label_dir / "manifest.json"
    tmp = label_dir / "manifest.json.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=False)
        f.write("\n")
    os.replace(tmp, manifest_path)
    return manifest


def _safe_name(name):
    name = re.sub(r"[^a-zA-Z0-9._-]+", "-", str(name)).strip("-.").lower()
    return name or "scene"


def _unique_name(name, used):
    if name not in used:
        return name
    n = 2
    while f"{name}-{n}" in used:
        n += 1
    return f"{name}-{n}"


# --------------------------------------------------------------------------------------------------
# perceptual features and metrics (Pillow only; no numpy)
# --------------------------------------------------------------------------------------------------

def _edge_map(luma, w, h):
    """Gradient magnitude on the thumbnail: UI text/layout is static even when the mist behind moves."""
    out = [0.0] * (w * h)
    for y in range(h):
        row = y * w
        down = row + w
        for x in range(w):
            i = row + x
            gx = abs(luma[i + 1] - luma[i]) if x + 1 < w else 0.0
            gy = abs(luma[down + x] - luma[i]) if y + 1 < h else 0.0
            out[i] = gx + gy
    return out


def _features_from_rgb(im, thumb_w=160, thumb_h=90):
    small = im.resize((thumb_w, thumb_h), Image.BILINEAR)
    ycc = small.convert("YCbCr")
    y = ycc.split()[0]
    luma = _pixels(y)
    cw, ch = max(1, thumb_w // 2), max(1, thumb_h // 2)
    small2 = im.resize((cw, ch), Image.BILINEAR).convert("YCbCr")
    cb_ch, cr_ch = small2.split()[1], small2.split()[2]
    cb_list, cr_list = _pixels(cb_ch), _pixels(cr_ch)
    n = len(luma)
    mean = _mean(luma)
    var = sum((v - mean) ** 2 for v in luma) / n
    sat = _mean([((a - 128) ** 2 + (b - 128) ** 2) ** 0.5 for a, b in zip(cb_list, cr_list)])
    return {
        "size": list(im.size),
        "luma": luma,
        "edge": _edge_map(luma, thumb_w, thumb_h),
        "luma_w": thumb_w,
        "luma_h": thumb_h,
        "cb": cb_list,
        "cr": cr_list,
        "chroma_w": cw,
        "chroma_h": ch,
        "mean_luma": mean,
        "luma_std": var ** 0.5,
        "saturation": sat,
        "black_share": sum(1 for v in luma if v < 12) / n,
        "bright_share": sum(1 for v in luma if v > 235) / n,
    }


def analyze_rgb(im, thumb=(160, 90)):
    return _features_from_rgb(im, thumb[0], thumb[1])


def analyze_path(path):
    return analyze_rgb(open_rgb(path))


def _check_feat(im, cfg):
    """Feature arrays at the configured check resolution, used for the perceptual metrics."""
    ck = (cfg or DEFAULT_CONFIG_DATA)["check"]
    return analyze_rgb(im, (int(ck["width"]), int(ck["height"])))


def _ssim_blocks(a, b, w, h, block=8):
    """Mean block SSIM of two 8-bit luma lists (w*h), non-overlapping `block` squares."""
    gw, gh = w // block, h // block
    if not gw or not gh:
        return 1.0
    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2
    total = 0.0
    count = 0
    for by in range(gh):
        for bx in range(gw):
            base = by * block * w + bx * block
            sa = sb = saa = sbb = sab = 0.0
            n = 0
            for yy in range(block):
                row = base + yy * w
                for xx in range(block):
                    va = a[row + xx]
                    vb = b[row + xx]
                    sa += va
                    sb += vb
                    saa += va * va
                    sbb += vb * vb
                    sab += va * vb
                    n += 1
            ma, mb = sa / n, sb / n
            va = saa / n - ma * ma
            vb = sbb / n - mb * mb
            cov = sab / n - ma * mb
            total += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
            count += 1
    return total / count


def _tile_metrics(ga, ra, w, h, cols, rows):
    tw, th = w // cols, h // rows
    tiles = []
    for ty in range(rows):
        for tx in range(cols):
            g_sum = r_sum = g_sq = r_sq = diff = 0.0
            n = 0
            for yy in range(ty * th, (ty + 1) * th):
                row = yy * w
                for xx in range(tx * tw, (tx + 1) * tw):
                    g = ga[row + xx]
                    r = ra[row + xx]
                    g_sum += g
                    r_sum += r
                    g_sq += g * g
                    r_sq += r * r
                    diff += abs(g - r)
                    n += 1
            gm = g_sum / n
            rm = r_sum / n
            tiles.append({
                "tx": tx, "ty": ty,
                "x": tx * tw, "y": ty * th, "w": tw, "h": th,
                "luma_mae": diff / n,
                "golden_mean": gm,
                "run_mean": rm,
                "golden_std": max(0.0, g_sq / n - gm * gm) ** 0.5,
                "run_std": max(0.0, r_sq / n - rm * rm) ** 0.5,
            })
    return tiles


def _chroma_mae(ca, cb_, da, db):
    n = len(ca)
    if not n:
        return 0.0
    s = 0.0
    for i in range(n):
        s += abs(ca[i] - da[i]) + abs(cb_[i] - db[i])
    return s / (2 * n)


def _chroma_saturation(cb, cr):
    return _mean([((a - 128) ** 2 + (b - 128) ** 2) ** 0.5 for a, b in zip(cb, cr)])


def compare_metrics(golden_feat, run_feat, cfg):
    """Perceptual difference between a golden and a run frame, both prepared at the check size."""
    ck = cfg["check"] if cfg else DEFAULT_CONFIG_DATA["check"]
    cols, rows = ck.get("tiles", [8, 5])
    ga, ra = golden_feat["luma"], run_feat["luma"]
    if len(ga) != len(ra):
        raise VisualError("compare_metrics: luma sizes differ")
    n = len(ga)
    diffs = [abs(a - b) for a, b in zip(ga, ra)]
    luma_mae = sum(diffs) / n
    p99 = sorted(diffs)[min(n - 1, int(n * 0.99))]
    w, h = run_feat["luma_w"], run_feat["luma_h"]
    ssim = _ssim_blocks(ga, ra, w, h, 8)
    tiles = _tile_metrics(ga, ra, w, h, cols, rows)
    worst = max(tiles, key=lambda t: t["luma_mae"])
    chroma_mae = _chroma_mae(golden_feat["cb"], golden_feat["cr"], run_feat["cb"], run_feat["cr"])
    g_sat = _chroma_saturation(golden_feat["cb"], golden_feat["cr"])
    r_sat = _chroma_saturation(run_feat["cb"], run_feat["cr"])
    g_mean = _mean(ga)
    r_mean = _mean(ra)
    black_tiles = [
        t for t in tiles
        if t["golden_mean"] >= 35 and t["golden_std"] >= 12 and t["run_mean"] <= 10
    ]
    return {
        "luma_mae": round(luma_mae, 4),
        "p99_luma_diff": p99,
        "chroma_mae": round(chroma_mae, 4),
        "ssim": round(ssim, 5),
        "tile_luma_mae": round(worst["luma_mae"], 4),
        "tile_index": [worst["tx"], worst["ty"]],
        "tile_bbox": [worst["x"], worst["y"], worst["w"], worst["h"]],
        "golden_saturation": round(g_sat, 4),
        "run_saturation": round(r_sat, 4),
        "sat_ratio": round(r_sat / g_sat, 5) if g_sat > 1e-6 else None,
        "mean_luma_ratio": round(r_mean / g_mean, 5) if g_mean > 1e-6 else None,
        "run_mean_luma": round(r_mean, 3),
        "golden_mean_luma": round(g_mean, 3),
        "black_share": round(sum(1 for v in ra if v < 12) / n, 4),
        "black_tiles": [{"tile": [t["tx"], t["ty"]], "bbox": [t["x"], t["y"], t["w"], t["h"]],
                         "run_mean": round(t["run_mean"], 2)} for t in black_tiles[:8]],
        "black_tile_count": len(black_tiles),
    }


class CheckedFrame:
    """A frame resized/analysed at the check resolution, cached because matching touches every frame."""

    def __init__(self, path, cfg):
        self.path = Path(path)
        self.im = open_rgb(path)
        ck = cfg["check"]
        w, h = int(ck["width"]), int(ck["height"])
        resized = self.im.resize((w, h), Image.BILINEAR)
        ycc = resized.convert("YCbCr")
        self.luma = _pixels(ycc.split()[0])
        half = resized.resize((max(1, w // 2), max(1, h // 2)), Image.BILINEAR).convert("YCbCr")
        self.cb = _pixels(half.split()[1])
        self.cr = _pixels(half.split()[2])
        self.size = self.im.size
        self.feat = analyze_rgb(self.im)  # 160x90 thumbnail: cheap scene matching
        self.check_feat = {
            "size": list(self.im.size),
            "luma": self.luma,
            "luma_w": w,
            "luma_h": h,
            "cb": self.cb,
            "cr": self.cr,
            "mean_luma": _mean(self.luma),
            "black_share": sum(1 for v in self.luma if v < 12) / len(self.luma),
        }


def match_frame(run_feat, golden_feat, cfg):
    """Can this run frame be the golden scene? Cheap thumbnail test with a structural check."""
    m = cfg["check"]["match"]
    ga, ra = golden_feat["luma"], run_feat["luma"]
    n = len(ga)
    if len(ra) != n:
        return {"matched": False, "quality": 1e9, "luma_mae": 1e9, "chroma_mae": 1e9, "ssim": 0.0}
    luma_mae = sum(abs(a - b) for a, b in zip(ga, ra)) / n
    if luma_mae > m["luma_mae_max"] * 2.5:
        return {"matched": False, "quality": 1e9, "luma_mae": luma_mae, "chroma_mae": None, "ssim": None}
    chroma_mae = _chroma_mae(golden_feat["cb"], golden_feat["cr"], run_feat["cb"], run_feat["cr"])
    edge_mae = thumb_mae(golden_feat.get("edge", ga), run_feat.get("edge", ra))
    ssim = _ssim_blocks(ga, ra, run_feat["luma_w"], run_feat["luma_h"], 8)
    quality = luma_mae + 0.5 * chroma_mae + 0.5 * edge_mae + 60.0 * (1.0 - ssim)
    matched = (luma_mae <= m["luma_mae_max"] and chroma_mae <= m["chroma_mae_max"]
               and edge_mae <= m.get("edge_mae_max", 6.0) and ssim >= m["ssim_min"])
    return {"matched": matched, "quality": quality, "luma_mae": luma_mae,
            "chroma_mae": chroma_mae, "edge_mae": edge_mae, "ssim": ssim}


def effective_thresholds(scene, cfg):
    th = dict(cfg["check"]["thresholds"])
    th.update(cfg.get("scenes", {}).get(scene, {}).get("thresholds", {}))
    return th


def is_required(scene, cfg, label):
    sc = cfg.get("scenes", {}).get(scene, {})
    if "required" in sc:
        return bool(sc["required"])
    lab = cfg.get("labels", {}).get(label, {})
    if scene in lab.get("required", []):
        return True
    if scene in lab.get("optional", []):
        return False
    return bool(lab.get("default_required", True))


def _evaluate(scene, metrics, cfg, allow_resize, golden_size, run_size):
    th = effective_thresholds(scene, cfg)
    reasons = []
    if tuple(golden_size) != tuple(run_size) and not allow_resize:
        reasons.append(f"resolution {run_size[0]}x{run_size[1]} != golden {golden_size[0]}x{golden_size[1]} "
                       f"(pass --allow-resize to compare anyway)")
    if metrics["luma_mae"] > th["luma_mae_max"]:
        reasons.append(f"luma MAE {metrics['luma_mae']:.2f} > {th['luma_mae_max']}")
    if metrics["chroma_mae"] > th["chroma_mae_max"]:
        reasons.append(f"chroma MAE {metrics['chroma_mae']:.2f} > {th['chroma_mae_max']}")
    if metrics["ssim"] < th["ssim_min"]:
        reasons.append(f"SSIM {metrics['ssim']:.3f} < {th['ssim_min']}")
    if metrics["tile_luma_mae"] > th["tile_luma_mae_max"]:
        tx, ty = metrics["tile_index"]
        reasons.append(f"worst tile {metrics['tile_luma_mae']:.2f} > {th['tile_luma_mae_max']} at tile ({tx},{ty})")
    if metrics["black_tile_count"] > th["max_black_tiles"]:
        t = metrics["black_tiles"][0]
        reasons.append(f"{metrics['black_tile_count']} black tile(s) where the golden has detail "
                       f"(first bbox {t['bbox']})")
    if metrics["run_mean_luma"] < th["min_mean_luma"]:
        reasons.append(f"black frame (mean luma {metrics['run_mean_luma']:.2f} < {th['min_mean_luma']})")
    if metrics["black_share"] > th["black_share_max"]:
        reasons.append(f"black share {metrics['black_share']:.2f} > {th['black_share_max']}")
    g_sat = metrics["golden_saturation"]
    r_sat = metrics["run_saturation"]
    if g_sat >= 3.0:
        if r_sat < g_sat * th["sat_ratio_min"]:
            reasons.append(f"chroma collapse: saturation {r_sat:.2f} < {g_sat * th['sat_ratio_min']:.2f} "
                           f"({r_sat / g_sat:.2f} of golden)")
    elif g_sat > 0.2 and r_sat < g_sat * 0.25:
        reasons.append(f"chroma collapse: saturation {r_sat:.2f} vs golden {g_sat:.2f}")
    if metrics["mean_luma_ratio"] is not None:
        if metrics["mean_luma_ratio"] < th["mean_luma_ratio_min"]:
            reasons.append(f"mean luma ratio {metrics['mean_luma_ratio']:.2f} < {th['mean_luma_ratio_min']}")
        elif metrics["mean_luma_ratio"] > th["mean_luma_ratio_max"]:
            reasons.append(f"mean luma ratio {metrics['mean_luma_ratio']:.2f} > {th['mean_luma_ratio_max']}")
    return reasons


# --------------------------------------------------------------------------------------------------
# capture helpers: stability and scene extraction
# --------------------------------------------------------------------------------------------------

def thumb_mae(a, b):
    if len(a) != len(b):
        return 1e9
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def thumb_diff(a, b):
    """(luma MAE, chroma MAE, edge MAE) between two analyze_rgb() features."""
    return (thumb_mae(a["luma"], b["luma"]),
            _chroma_mae(a["cb"], a["cr"], b["cb"], b["cr"]),
            thumb_mae(a.get("edge", a["luma"]), b.get("edge", b["luma"])))


def frames_similar(a, b, cfg):
    """Same screen? The mist/embers behind a menu may move a lot while its text edges stay put."""
    cap = cfg["capture"]
    lm, cm, em = thumb_diff(a, b)
    return (em <= float(cap["stable_edge_mae_max"])
            and cm <= float(cap["stable_chroma_mae_max"])
            and lm <= float(cap["stable_luma_mae_max"]))


def _is_black(feat):
    # a fade is nearly uniform; the brightness card is dark but has UI detail (spread ~11), so it is
    # a screen that takes A, not a dead frame that takes PLUS.
    return feat["mean_luma"] < 8.0 and feat["black_share"] > 0.90 and feat["luma_std"] < 10.0


def classify_kind(feat, motion=None):
    mean, spread, sat, black = feat["mean_luma"], feat["luma_std"], feat["saturation"], feat["black_share"]
    if _is_black(feat):
        return "black"
    if motion is not None and motion > 4.0:
        return "dynamic"
    if mean >= 170 and sat <= 10 and black <= 0.05:
        return "bright"
    if mean <= 110 and spread <= 45 and black <= 0.70:
        return "dark-ui"
    if 95 <= mean <= 210 and sat >= 8 and spread >= 45:
        return "mid-ui"
    if mean < 80 and sat >= 8 and spread >= 18:
        return "splash"
    return "scene"


def _heuristic_name(feat, used):
    mean, spread, sat, black = feat["mean_luma"], feat["luma_std"], feat["saturation"], feat["black_share"]

    def take(name):
        return None if name in used else name

    if mean >= 170 and sat <= 10 and black <= 0.05:
        for name in ("title", "loading", "bright-screen"):
            got = take(name)
            if got:
                return got
    if mean < 80 and sat >= 8 and spread >= 18 and black < 0.6:
        for name in ("logo", "splash-card"):
            got = take(name)
            if got:
                return got
    if 90 <= mean <= 210 and sat >= 6 and spread >= 40:
        for name in ("chapter-select", "mid-screen"):
            got = take(name)
            if got:
                return got
    if mean <= 110 and spread <= 45 and black <= 0.70:
        if sat <= 1.5:
            for name in ("main-menu", "menu-screen"):
                got = take(name)
                if got:
                    return got
        for name in ("option-card", "card"):
            got = take(name)
            if got:
                return got
    if 40 <= mean < 110:
        for name in ("dark-scene", "bright-card"):
            got = take(name)
            if got:
                return got
    if mean < 15 and spread >= 8 and black >= 0.75:
        for name in ("brightness-card", "dark-card", "fade"):
            got = take(name)
            if got:
                return got
    return _unique_name("scene", used)


def extract_scenes(frames, cfg, hints=None):
    """Find stable stretches in a frames dir and one representative golden per distinct scene.

    Frames are identified through their neighbours only, so this works for dense capture runs and
    for sparse frame dumps. `hints` is a loaded golden store: when given, frames are named after the
    golden they match, so a second run labels the same screens the same way.
    """
    cap = cfg["capture"]
    paths = list_frames(frames)
    if not paths:
        raise VisualError(f"no frames to extract scenes from in {frames}")
    feats = []
    for p in paths:
        try:
            feats.append(analyze_path(p))
        except Exception as exc:
            raise VisualError(f"could not read frame {p}: {exc}") from exc
    min_samples = int(cap["min_stable_samples"])
    dedupe_edge = float(cap["dedupe_edge_mae_max"])
    dedupe_luma = float(cap["dedupe_luma_mae_max"])

    runs = []
    start = 0
    for i in range(1, len(paths)):
        if frames_similar(feats[i - 1], feats[i], cfg):
            continue
        runs.append((start, i - 1))
        start = i
    runs.append((start, len(paths) - 1))

    candidates = []
    used_names = set()
    accepted = []
    for (a, b) in runs:
        if b - a + 1 < min_samples:
            continue
        run_feats = feats[a:b + 1]
        if max(f["mean_luma"] for f in run_feats) > 150:
            mid = a + max(range(len(run_feats)), key=lambda k: run_feats[k]["mean_luma"])
        else:
            mid = (a + b) // 2
        feat = feats[mid]
        if _is_black(feat):
            continue
        duplicate = False
        for other in accepted:
            lm, cm, em = thumb_diff(other["feat"], feat)
            if em <= dedupe_edge and lm <= dedupe_luma:
                other["stable_samples"] = max(other["stable_samples"], b - a + 1)
                duplicate = True
                break
        if duplicate:
            continue
        if len(accepted) >= int(cap["max_scenes"]):
            break
        name = None
        if hints:
            best = None
            for golden in hints["scenes"]:
                if golden["scene"] in used_names:
                    continue
                m = match_frame(feat, golden["feat"], cfg)
                if m["matched"] and (best is None or m["quality"] < best[0]):
                    best = (m["quality"], golden["scene"])
            if best:
                name = best[1]
        if name is None:
            name = _heuristic_name(feat, used_names)
        if name in used_names:
            name = _unique_name(name, used_names)
        used_names.add(name)
        accepted.append({
            "name": name,
            "feat": feat,
            "path": str(paths[mid]),
            "frame": frame_number(paths[mid]),
            "present": frame_number(paths[mid]),
            "im": open_rgb(paths[mid]),
            "mean_luma": feat["mean_luma"],
            "luma_std": feat["luma_std"],
            "saturation": feat["saturation"],
            "black_share": feat["black_share"],
            "bright_share": feat["bright_share"],
            "stable_samples": b - a + 1,
            "source_frame": str(paths[mid]),
        })
    candidates = accepted

    dynamic = []
    motion = [None]
    for i in range(1, len(feats)):
        motion.append(thumb_mae(feats[i - 1]["luma"], feats[i]["luma"]))
    movie_frames = [i for i, m in enumerate(motion)
                    if m is not None and m > 4.0 and feats[i]["saturation"] > 5.0 and feats[i]["mean_luma"] > 15]
    if movie_frames:
        dynamic.append({"kind": "movie",
                        "first_frame": frame_number(paths[movie_frames[0]]),
                        "last_frame": frame_number(paths[movie_frames[-1]]),
                        "samples": len(movie_frames)})
    black_runs = []
    i = 0
    while i < len(feats):
        if _is_black(feats[i]):
            j = i
            while j + 1 < len(feats) and _is_black(feats[j + 1]):
                j += 1
            if j > i:
                black_runs.append({"first_frame": frame_number(paths[i]),
                                   "last_frame": frame_number(paths[j]), "samples": j - i + 1})
            i = j + 1
        else:
            i += 1
    if black_runs:
        dynamic.append({"kind": "black-fades", "runs": black_runs[:8]})
    return {"scenes": candidates, "dynamic": dynamic, "frames": len(paths)}


# --------------------------------------------------------------------------------------------------
# capture: drive the game headless, sample frames, adopt goldens
# --------------------------------------------------------------------------------------------------

def _terminate_tree(proc, grace=6.0):
    if proc.poll() is not None:
        return
    try:
        if os.name != "nt":
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        else:
            proc.terminate()
    except (ProcessLookupError, PermissionError, OSError):
        pass
    end = time.time() + grace
    while time.time() < end and proc.poll() is None:
        time.sleep(0.1)
    if proc.poll() is None:
        try:
            if os.name != "nt":
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            else:
                proc.kill()
        except (ProcessLookupError, PermissionError, OSError):
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def _write_live(path, button, settle=0.22):
    path = str(path)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        if button:
            f.write(f"0:{button}:0;")
    os.replace(tmp, path)
    if button:
        time.sleep(settle)
        with open(path, "w", encoding="utf-8") as f:
            f.write("")


def _request_frame(root, frames_dir, floor, timeout):
    marker = Path(root) / "dump_frames"
    try:
        with open(marker, "w", encoding="utf-8") as f:
            f.write("1 1")
    except OSError:
        return None
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(0.12)
        best = None
        for p in Path(frames_dir).glob("[0-9][0-9][0-9][0-9][0-9].ppm"):
            n = frame_number(p)
            if n is not None and n > floor and (best is None or n > best[0]):
                best = (n, p)
        if best:
            return best
    return None


def _clean_frames(frames_dir):
    frames_dir = Path(frames_dir)
    frames_dir.mkdir(parents=True, exist_ok=True)
    removed = 0
    for pattern in ("*.ppm", "*.png", "*.jpg", "*.jpeg"):
        for p in frames_dir.glob(pattern):
            try:
                p.unlink()
                removed += 1
            except OSError:
                pass
    return removed


def perform_capture(args, cfg, label):
    """Run the game headless, sample frames on demand, return the run's scene candidates.

    Phase 1 (boot) sends no input at all: the game reaches PRESS ANY BUTTON on its own, which is the
    deterministic part of the flow. Phase 2 only starts at the title screen and drives the menus with
    the live input file. A run that never renders a title (drivers/shader cache/GPU contention) raises
    CaptureEmpty -- never a visual mismatch, and never a golden set.
    """
    root = Path(args.data_root or os.environ.get("NX_DATA_ROOT") or "private/run-bayonetta3").expanduser()
    if not root.exists():
        raise VisualError(f"data root does not exist: {root}")
    frames_dir = root / "frames"
    removed = _clean_frames(frames_dir)
    exe = args.exe or os.environ.get("NX_VISUAL_EXE")
    if not exe:
        for guess in (REPO / "private" / "build" / "bayonetta3",
                      REPO / "private" / "build-mac" / "bayonetta3"):
            if guess.exists():
                exe = str(guess)
                break
    if not exe or not Path(exe).exists():
        raise VisualError("no game executable: pass --exe (or set NX_VISUAL_EXE)")
    live = root / "input_live.txt"
    live.write_text("", encoding="utf-8")
    report_dir = Path(args.report_dir) if args.report_dir else label_dir_for(label, cfg, args.goldens_root) / "captures"
    report_dir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = report_dir / f"game-{stamp}.log"

    env = dict(os.environ)
    env.update({
        "NX_DATA_ROOT": str(root),
        "NX_HEADLESS": "1",
        "NX_MUTE": "1",
        "NX_INPUT_LIVE": str(live),
        "NX_LOG": str(log_path),
    })
    env.setdefault("NX_MEM_LIMIT_GB", "8")
    env.pop("NX_DUMP_FRAMES", None)
    env.pop("NX_INPUT_SCRIPT", None)
    tracked_env = {k: env.get(k) for k in sorted(env) if k.startswith("NX_")}
    argv = [str(exe)]
    contention = other_game_processes()
    capture_meta = {
        "label": label,
        "exe": str(exe),
        "exe_sha256": sha256_file(exe),
        "argv": argv,
        "env": tracked_env,
        "data_root": str(root),
        "frames_dir": str(frames_dir),
        "seconds": args.seconds,
        "started": iso_now(),
        "platform": platform_info(),
        "frames_cleaned": removed,
        "contention": {"free": not contention, "other_bayonetta3": contention},
        "goal_file": str(root / "goal"),
        "goal_env_override": bool(env.get("NX_GOAL")),
    }
    if not args.quiet:
        print(f"capture: label={label} exe={exe}", flush=True)
        print(f"capture: data-root={root} frames={frames_dir} seconds={args.seconds}", flush=True)
        if contention:
            print(f"capture: WARNING: other bayonetta3 process(es) running; timing may vary, "
                  f"but the scene set should not: {contention}", flush=True)

    cap = cfg["capture"]
    every = float(args.every or cap["every"])
    seconds = float(args.seconds or cap["seconds"])
    settle = float(cap["settle"])
    write_goal(root, phase="boot", scene="boot", step=0, frame=0, elapsed_s=0,
               label=label, goal="boot with no input; wait for the title screen")
    stdout = open(log_path, "ab")
    proc = subprocess.Popen(argv, env=env, stdout=stdout, stderr=subprocess.STDOUT,
                            start_new_session=(os.name != "nt"),
                            creationflags=(subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0))
    old_handlers = {}

    def _stop_on_signal(signum, _frame):
        raise KeyboardInterrupt

    if os.name != "nt":
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                old_handlers[sig] = signal.signal(sig, _stop_on_signal)
            except (ValueError, OSError):
                pass
    samples = []
    floor = 0
    t0 = time.time()
    misses = 0
    screen_samples = 0
    press_index = 0
    since_press = 0
    prev_feat = None
    black_streak = 0
    title_seen = False
    phase = "boot"
    goal_scene = "boot"
    goal_step = 0
    goal_names = set()
    last_goal_feat = None
    last_goal_write = 0.0
    title_dwell = 0
    error = None
    try:
        while True:
            now = time.time()
            if proc.poll() is not None:
                error = f"game exited (code {proc.returncode}) after {now - t0:.0f} s; see {log_path}"
                break
            if now - t0 > seconds:
                break
            if misses and now - t0 > float(cap["boot_timeout"]):
                error = f"no frame within {cap['boot_timeout']} s; see {log_path}"
                break
            if not title_seen and now - t0 > float(cap.get("title_timeout", 120.0)):
                error = (f"no title screen within {cap.get('title_timeout', 120)} s "
                         f"(the boot flow did not reach PRESS ANY BUTTON); see {log_path}")
                break
            got = _request_frame(root, frames_dir, floor, float(cap["frame_timeout"]))
            if got is None:
                misses += 1
                if misses > int(float(cap["no_frame_timeout"]) / max(0.5, every)):
                    error = f"no new frame for {cap['no_frame_timeout']} s; see {log_path}"
                    break
                continue
            misses = 0
            floor, path = got
            try:
                im = open_rgb(path)
            except Exception as exc:
                if not args.quiet:
                    print(f"  skip unreadable frame {path.name}: {exc}", flush=True)
                continue
            feat = analyze_rgb(im)
            motion = None
            edge_change = None
            if prev_feat is not None:
                motion = thumb_mae(prev_feat["luma"], feat["luma"])
                edge_change = thumb_mae(prev_feat["edge"], feat["edge"])
            kind = classify_kind(feat, motion)
            changed = (prev_feat is None
                       or (edge_change is not None and edge_change > float(cap["screen_change_edge"]))
                       or (motion is not None and motion > float(cap["screen_change_luma"])))
            if changed:
                screen_samples = 0
                press_index = 0
            else:
                screen_samples += 1
            samples.append({
                "frame": floor, "path": str(path), "kind": kind,
                "mean_luma": round(feat["mean_luma"], 2),
                "luma_std": round(feat["luma_std"], 2),
                "saturation": round(feat["saturation"], 2),
                "black_share": round(feat["black_share"], 4),
                "motion": None if motion is None else round(motion, 2),
            })
            # Phase 1: no input until the title screen has been stable for two samples.
            if not title_seen and screen_samples >= 2 and feat["mean_luma"] >= 170 \
                    and feat["saturation"] <= 10 and feat["black_share"] <= 0.05:
                title_seen = True
                phase = "drive"
                goal_scene = "title"
                goal_step = 1
                goal_names = {"title"}
                last_goal_feat = feat
                since_press = 0
                title_dwell = int(cap.get("title_dwell_samples", 4))
                write_goal(root, phase=phase, scene="title", step=goal_step, frame=floor,
                           elapsed_s=round(now - t0, 1), label=label,
                           goal="title reached; drive the menus with the live input file")
            # Phase 2: capture the current stable scene for the goal file (names are provisional
            # until extract_scenes() names the adopted goldens).
            if title_seen and screen_samples >= 2:
                if last_goal_feat is None:
                    last_goal_feat = feat
                else:
                    lm, cm, em = thumb_diff(last_goal_feat, feat)
                    if em > float(cap["dedupe_edge_mae_max"]) or lm > float(cap["dedupe_luma_mae_max"]):
                        goal_scene = _heuristic_name(feat, goal_names)
                        goal_names.add(goal_scene)
                        goal_step += 1
                        last_goal_feat = feat
                        write_goal(root, phase=phase, scene=goal_scene, step=goal_step, frame=floor,
                                   elapsed_s=round(now - t0, 1), label=label,
                                   goal="stable scene; advancing")
            if now - last_goal_write >= 2.0:
                write_goal(root, phase=phase, scene=goal_scene, step=goal_step, frame=floor,
                           elapsed_s=round(now - t0, 1), kind=kind, label=label,
                           goal="visual capture in progress")
                last_goal_write = now
            button = None
            if title_dwell > 0:
                title_dwell -= 1  # collect enough title samples before advancing: it pulses
                button = None
            elif not title_seen:
                button = None  # the boot flow is deterministic: no input at all before the title
            elif kind == "black":
                black_streak += 1
                if black_streak >= 3 and black_streak % 5 == 0:
                    button = "PLUS"
            else:
                black_streak = 0
                if screen_samples >= 2:
                    sequence = ["A", "A", "B", "DOWN", "A", "A", "UP", "A"]
                    button = sequence[min(press_index, len(sequence) - 1)]
                    press_index += 1
            if button is None and since_press >= int(cap["max_dwell_samples"]) and kind != "black" \
                    and title_seen:
                # An animated screen whose edges do not settle (or a prompt that ignores the usual
                # button): do not sit on it forever.
                button = "A"
                press_index += 1
            since_press = 0 if button else since_press + 1
            if not args.quiet:
                print(f"  {floor:05d} luma {feat['mean_luma']:6.1f} spread {feat['luma_std']:5.1f} "
                      f"sat {feat['saturation']:5.1f} black {feat['black_share']:.2f} "
                      f"motion {'-' if motion is None else f'{motion:5.1f}'} {kind:8s} "
                      f"{('press ' + button) if button else ''}", flush=True)
            if button:
                _write_live(live, button, settle)
            prev_feat = feat
            remaining = every - (time.time() - now)
            if remaining > 0:
                time.sleep(remaining)
    except KeyboardInterrupt:
        error = "interrupted; the game was stopped"
    finally:
        _write_live(live, "")
        _terminate_tree(proc)
        for sig, handler in old_handlers.items():
            try:
                signal.signal(sig, handler)
            except (ValueError, OSError):
                pass
        stdout.close()
        capture_meta["finished"] = iso_now()
        capture_meta["elapsed_s"] = round(time.time() - t0, 1)
        capture_meta["samples"] = len(samples)
        capture_meta["last_frame"] = samples[-1]["frame"] if samples else None
        capture_meta["log"] = str(log_path)
        capture_meta["game_exit"] = proc.returncode
        capture_meta["title_seen"] = title_seen

    if error:
        write_goal(root, phase="failed", scene=goal_scene, step=goal_step, frame=floor,
                   elapsed_s=round(time.time() - t0, 1), label=label, goal=error[:150])
        raise CaptureEmpty(f"{error}\n--- last game log lines ---\n{_log_tail(log_path)}")
    if not title_seen:
        write_goal(root, phase="failed", scene="none", step=0, frame=floor, label=label,
                   goal="no title screen: no input before it, so the boot flow is not rendering")
        raise CaptureEmpty(f"no title screen in {len(samples)} sampled frames (all black/no usable scene). "
                           f"See {log_path}\n{_log_tail(log_path)}")
    if len(samples) < 4:
        raise CaptureEmpty(f"capture got only {len(samples)} frame(s); the game did not render. "
                           f"See {log_path}\n{_log_tail(log_path)}")

    existing = load_goldens(label_dir_for(label, cfg, args.goldens_root), cfg)
    extraction = extract_scenes(frames_dir, cfg, existing)
    if not extraction["scenes"]:
        write_goal(root, phase="failed", scene=goal_scene, step=goal_step, frame=floor, label=label,
                   goal="no stable scene found after the title")
        raise CaptureEmpty(f"no stable scene found in {extraction['frames']} frames after the title; "
                           f"see {log_path}")
    names = [sc["name"] for sc in extraction["scenes"]]
    if "title" not in names:
        raise CaptureEmpty(f"the title screen was seen but not recovered as a stable golden "
                           f"(scenes: {names}); treat this run as a failed capture")
    capture_meta["frames_total"] = extraction["frames"]
    capture_meta["scenes"] = [
        {"scene": sc["name"], "frame": sc["frame"], "stable_samples": sc["stable_samples"],
         "mean_luma": round(sc["mean_luma"], 2), "luma_std": round(sc["luma_std"], 2),
         "saturation": round(sc["saturation"], 2), "black_share": round(sc["black_share"], 4)}
        for sc in extraction["scenes"]
    ]
    capture_meta["dynamic"] = extraction["dynamic"]
    write_goal(root, phase="captured", scene=",".join(names), step=goal_step, frame=floor,
               elapsed_s=round(time.time() - t0, 1), label=label,
               goal=f"capture done: {len(names)} stable scene(s)")
    return {
        "meta": capture_meta,
        "candidates": extraction["scenes"],
        "dynamic": extraction["dynamic"],
        "frames_dir": frames_dir,
        "samples": samples,
        "report_dir": report_dir,
        "stamp": stamp,
    }


def _capture_with_retries(args, cfg, label):
    attempts = max(1, int(getattr(args, "retries", 1) or 1) + 1)
    last = None
    for attempt in range(attempts):
        try:
            return perform_capture(args, cfg, label)
        except CaptureEmpty as exc:
            last = exc
            if attempt + 1 >= attempts:
                break
            print(f"capture: no usable scene (attempt {attempt + 1}/{attempts}): {exc}")
            print("capture: retrying in 5 s (GPU contention or a bad boot can do this)")
            time.sleep(5)
    raise last


def _log_tail(path, lines=25):
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return "(no log)"
    return "\n".join(text[-lines:])


def adopt_capture(capture, cfg, args, label, note="capture"):
    candidates = capture["candidates"]
    if not candidates:
        raise VisualError("no stable scenes found to adopt: try a longer --seconds or inspect the game log")
    scenes = [{"name": c["name"], "im": c["im"], "frame": c["frame"], "present": c["present"],
               "mean_luma": c["mean_luma"], "luma_std": c["luma_std"], "saturation": c["saturation"],
               "black_share": c["black_share"], "bright_share": c["bright_share"],
               "stable_samples": c["stable_samples"], "source_frame": c["source_frame"],
               "captured_at": iso_now(), "meta": {"captured_by": note}} for c in candidates]
    meta = capture["meta"]
    manifest = {
        "label": label,
        "created": iso_now(),
        "created_by": f"{getpass.getuser()}@{platform.node()}",
        "authorship": note,
        "note": getattr(args, "note", None) or "",
        "exe": meta["exe"],
        "exe_sha256": meta["exe_sha256"],
        "argv": meta["argv"],
        "env": meta["env"],
        "platform": meta["platform"],
        "data_root": meta["data_root"],
        "frames_dir": meta["frames_dir"],
        "config_path": cfg.get("_config_path"),
        "config_sha256": cfg.get("_config_sha256"),
        "dynamic": capture["dynamic"],
        "capture": {k: v for k, v in meta.items() if k not in ("platform", "env")},
    }
    label_dir = label_dir_for(label, cfg, args.goldens_root)
    manifest = write_goldens(label_dir, scenes, manifest)
    return label_dir, manifest


# --------------------------------------------------------------------------------------------------
# check: match scenes to frames, measure, report, draw heatmaps
# --------------------------------------------------------------------------------------------------

def _align(golden_scenes, checked, cfg):
    """Assign each golden to one run frame, in order, allowing frames and scenes to be skipped.

    A scene whose best match is under the match thresholds is reported missing. The order
    constraint is what keeps two similar bright screens (title, loading) apart.
    """
    m = cfg["check"]["match"]
    penalty = float(m.get("missing_penalty", 60.0))
    n, k = len(golden_scenes), len(checked)
    costs = []
    for golden in golden_scenes:
        costs.append([match_frame(frame.feat, golden["feat"], cfg) for frame in checked])
    inf = 1e18
    dp = [[inf] * (k + 1) for _ in range(n + 1)]
    back = [[None] * (k + 1) for _ in range(n + 1)]
    for j in range(k + 1):
        dp[0][j] = 0.0
        back[0][j] = ("skip", j - 1) if j > 0 else None
    for i in range(1, n + 1):
        dp[i][0] = dp[i - 1][0] + penalty
        back[i][0] = ("missing", 0)
        for j in range(1, k + 1):
            best = dp[i][j - 1]
            move = ("skip", j - 1)
            cost = costs[i - 1][j - 1]
            if cost["matched"] and dp[i - 1][j - 1] + cost["quality"] < best:
                best = dp[i - 1][j - 1] + cost["quality"]
                move = ("match", j - 1)
            if dp[i - 1][j] + penalty < best:
                best = dp[i - 1][j] + penalty
                move = ("missing", j)
            dp[i][j] = best
            back[i][j] = move
    assigned = [None] * n
    i, j = n, k
    while i > 0 and j >= 0:
        move = back[i][j]
        if move is None:
            break
        kind, nj = move
        if kind == "skip":
            j = nj
        elif kind == "match":
            assigned[i - 1] = checked[nj]
            i, j = i - 1, nj
        else:
            i = i - 1
            j = min(j, nj)
    return assigned


def check_frames(frames_dir, store, cfg, label, allow_resize=False, report_dir=None, quiet=False):
    frames = list_frames(frames_dir)
    if not frames:
        raise VisualError(f"no frames in {frames_dir}")
    checked = []
    for p in frames:
        try:
            checked.append(CheckedFrame(p, cfg))
        except Exception as exc:
            if not quiet:
                print(f"warning: skipping unreadable frame {p.name}: {exc}", flush=True)
    if not checked:
        raise VisualError(f"no readable frames in {frames_dir}")
    assigned = _align(store["scenes"], checked, cfg)
    report_dir = Path(report_dir) if report_dir else None
    if report_dir:
        report_dir.mkdir(parents=True, exist_ok=True)
    scenes_report = []
    failures, warnings, failed_scenes = [], [], []
    for golden, frame in zip(store["scenes"], assigned):
        name = golden["scene"]
        required = is_required(name, cfg, label)
        if frame is None:
            scenes_report.append({"scene": name, "status": "missing", "required": required,
                                  "frame": None, "metrics": None,
                                  "reasons": ["scene not found in the run"]})
            if required:
                failures.append(f"{name}: missing from the run")
                failed_scenes.append(name)
            else:
                warnings.append(f"{name}: missing from this run (optional)")
            continue
        metrics = compare_metrics(golden["check_feat"], frame.check_feat, cfg)
        reasons = _evaluate(name, metrics, cfg, allow_resize,
                            golden["meta"]["resolution"], frame.size)
        status = "fail" if reasons else "pass"
        entry = {
            "scene": name,
            "status": status,
            "required": required,
            "frame": frame.path.name,
            "frame_number": frame_number(frame.path),
            "resolution": list(frame.size),
            "golden_resolution": golden["meta"]["resolution"],
            "metrics": metrics,
            "reasons": reasons,
        }
        scenes_report.append(entry)
        if reasons:
            failures.append(f"{name}: " + "; ".join(reasons))
            failed_scenes.append(name)
        if not quiet:
            print(_scene_line(entry), flush=True)
        if reasons and report_dir:
            heat = report_dir / f"visual-diff-{_safe_name(name)}.png"
            try:
                write_heatmap(golden["im"], frame.im, metrics, heat)
                entry["heatmap"] = str(heat)
            except Exception as exc:  # a missing heatmap must not hide the regression
                entry["heatmap_error"] = str(exc)
    assigned_ids = {id(f) for f in assigned if f is not None}
    for item in _extra_scenes(checked, assigned_ids, cfg):
        warnings.append(f"extra scene around frame {item['frame']} ({item['kind']}, "
                        f"{item['samples']} static samples)")
    all_black = all(f.feat["mean_luma"] < 8.0 and f.feat["black_share"] > 0.90 for f in checked)
    if all_black and not failures:
        failures.append("every frame of the run is black")
        failed_scenes.append("all-frames-black")
    ok = not failures
    passed = sum(1 for s in scenes_report if s["status"] == "pass")
    summary = (f"VISUAL REGRESS: {'PASS' if ok else 'FAIL'} "
               f"({passed}/{len(store['scenes'])} scenes passed"
               + (f"; failed: {', '.join(failed_scenes)}" if failed_scenes else "") + ")")
    return {
        "tool": TOOL,
        "version": VERSION,
        "ran": True,
        "ok": ok,
        "label": label,
        "summary": summary,
        "frames_dir": str(frames_dir),
        "frames": len(checked),
        "goldens_dir": store.get("label_dir"),
        "goldens_manifest": {"label": store["manifest"].get("label"),
                             "created": store["manifest"].get("created"),
                             "exe_sha256": store["manifest"].get("exe_sha256")},
        "platform": platform_info(),
        "timestamp": iso_now(),
        "scenes": scenes_report,
        "failed_scenes": failed_scenes,
        "failures": failures,
        "warnings": warnings,
        "config_path": cfg.get("_config_path"),
        "config_sha256": cfg.get("_config_sha256"),
    }


def _scene_line(entry):
    m = entry["metrics"]
    status = entry["status"].upper()
    line = (f"{status:4s} {entry['scene']:<16s} frame {str(entry['frame']):>12s}  "
            f"luma {m['luma_mae']:6.2f}  chroma {m['chroma_mae']:6.2f}  ssim {m['ssim']:.4f}  "
            f"tile {m['tile_luma_mae']:6.2f}@({m['tile_index'][0]},{m['tile_index'][1]})  "
            f"sat {m['sat_ratio'] if m['sat_ratio'] is not None else '-'}")
    if entry["status"] == "fail":
        line += "  <- " + "; ".join(entry["reasons"])
    return line


def _extra_scenes(checked, assigned_ids, cfg):
    still = float(cfg["capture"]["dedupe_edge_mae_max"])
    extras = []
    i = 0
    while i < len(checked):
        if id(checked[i]) in assigned_ids or _is_black(checked[i].feat) or checked[i].feat["mean_luma"] < 10:
            i += 1
            continue
        j = i
        while (j + 1 < len(checked) and id(checked[j + 1]) not in assigned_ids
               and thumb_mae(checked[j].feat.get("edge", checked[j].feat["luma"]),
                             checked[j + 1].feat.get("edge", checked[j + 1].feat["luma"])) <= still):
            j += 1
        if j - i + 1 >= 3:
            extras.append({"frame": frame_number(checked[i].path),
                           "kind": classify_kind(checked[i].feat),
                           "samples": j - i + 1})
        i = j + 1
    return extras


def write_heatmap(golden_im, run_im, metrics, out_path):
    """Amplified |luma difference| with the worst tile outlined, for a human reading a failure."""
    ck_w, ck_h = 480, 270
    g = golden_im.resize((ck_w, ck_h), Image.BILINEAR).convert("L")
    r = run_im.resize((ck_w, ck_h), Image.BILINEAR).convert("L")
    gb, rb = _luma_bytes(g), _luma_bytes(r)
    heat = bytes(min(255, abs(a - b) * 6) for a, b in zip(gb, rb))
    heat_im = Image.frombytes("L", (ck_w, ck_h), heat).convert("RGB")
    draw = ImageDraw.Draw(heat_im)
    x, y, w, h = metrics["tile_bbox"]
    draw.rectangle([x, y, x + w - 1, y + h - 1], outline=(255, 40, 40), width=2)
    draw.rectangle([0, 0, ck_w - 1, 12], fill=(0, 0, 0))
    draw.text((3, 1), f"luma MAE {metrics['luma_mae']:.2f}  SSIM {metrics['ssim']:.3f}  "
                      f"worst tile {metrics['tile_luma_mae']:.1f}", fill=(255, 255, 255))
    out_path = Path(out_path)
    tmp = out_path.with_suffix(".tmp.png")
    heat_im.save(tmp, format="PNG")
    os.replace(tmp, out_path)


# --------------------------------------------------------------------------------------------------
# subcommands
# --------------------------------------------------------------------------------------------------

def _common_paths(args, cfg, label):
    return label_dir_for(label, cfg, getattr(args, "goldens_root", None))


def cmd_capture(args):
    cfg = load_config(args.config)
    label = args.label or default_label()
    capture = _capture_with_retries(args, cfg, label)
    existed = load_goldens(_common_paths(args, cfg, label), cfg)
    adopt = args.adopt or (existed is None and not args.no_adopt)
    if adopt:
        label_dir, manifest = adopt_capture(capture, cfg, args, label,
                                            note="capture" + (" (forced --adopt)" if args.adopt else ""))
        print(f"capture: adopted {len(manifest['scenes'])} goldens -> {label_dir}")
        for s in manifest["scenes"]:
            print(f"  golden {s['scene']:<16s} frame {s['frame']} stable {s['stable_samples']} "
                  f"luma {s['mean_luma']:.1f} sat {s['saturation']:.1f}")
        return 0
    if args.no_adopt:
        print("capture: --no-adopt: kept the frames only (use bless to promote them)")
        return 0
    report = check_frames(capture["frames_dir"], existed, cfg, label,
                          allow_resize=False, report_dir=capture["report_dir"], quiet=args.quiet)
    calibration = {
        "label": label,
        "run": capture["meta"],
        "timestamp": iso_now(),
        "scenes": {s["scene"]: s.get("metrics") for s in report["scenes"] if s.get("metrics")},
    }
    _append_calibration(_common_paths(args, cfg, label), calibration)
    outcome = {
        "ran": True,
        "ok": report["ok"],
        "label": label,
        "capture": capture["meta"],
        "check": report,
        "goldens_dir": str(_common_paths(args, cfg, label)),
        "timestamp": iso_now(),
    }
    _write_json(Path(args.report_dir or capture["report_dir"]) / "visual-report.json", outcome)
    print(report["summary"])
    if not report["ok"]:
        print(f"capture: frames differ from the current goldens. If the change is intended, run "
              f"`visual_regress.py bless --from {capture['frames_dir']} --label {label} --note <why>`.")
        return 1
    return 0


def _append_calibration(label_dir, entry):
    label_dir.mkdir(parents=True, exist_ok=True)
    path = label_dir / "calibration.json"
    data = {"label": entry["label"], "samples": []}
    if path.exists():
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            data = {"label": entry["label"], "samples": []}
    data.setdefault("samples", []).append(entry)
    data["samples"] = data["samples"][-10:]
    tmp = label_dir / "calibration.json.tmp"
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def cmd_check(args):
    cfg = load_config(args.config)
    label = args.label or default_label()
    frames = Path(args.frames or os.path.join(os.environ.get("NX_DATA_ROOT", ""), "frames"))
    label_dir = _common_paths(args, cfg, label)
    store = load_goldens(label_dir, cfg)
    if store is None:
        print(f"visual check: no goldens for label {label!r} under {label_dir}; skipping "
              f"(capture them with: visual_regress.py capture --label {label} --exe <game>)")
        if args.json:
            _write_json(Path(args.json), {"ran": False, "ok": True, "skipped": True, "label": label,
                                          "reason": "no-goldens", "goldens_dir": str(label_dir),
                                          "timestamp": iso_now()})
        return 0
    report_dir = Path(args.report_dir) if args.report_dir else frames.parent / "visual-report"
    report = check_frames(frames, store, cfg, label, allow_resize=args.allow_resize,
                          report_dir=report_dir, quiet=args.quiet)
    print(report["summary"])
    report["report_path"] = str(report_dir / "report.json")
    _write_json(report_dir / "report.json", report)
    if args.json:
        _write_json(Path(args.json), report)
    return 0 if report["ok"] else 1


def cmd_bless(args):
    cfg = load_config(args.config)
    label = args.label or default_label()
    source = Path(args.source)
    label_dir = _common_paths(args, cfg, label)
    existing = load_goldens(label_dir, cfg)
    if source.is_dir() and (source / "manifest.json").is_file():
        src_manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
        scenes = []
        for meta in src_manifest.get("scenes", []):
            png = source / meta["file"]
            if not png.exists():
                raise VisualError(f"bless: {png} is missing")
            im = open_rgb(png)
            scenes.append({
                "name": meta["scene"], "im": im,
                "frame": meta.get("frame"), "present": meta.get("present"),
                "mean_luma": meta.get("mean_luma", _mean(_luma_bytes(im))),
                "luma_std": meta.get("luma_std", 0.0),
                "saturation": meta.get("saturation", 0.0),
                "black_share": meta.get("black_share", 0.0),
                "bright_share": meta.get("bright_share", 0.0),
                "stable_samples": meta.get("stable_samples"),
                "source_frame": meta.get("source_frame"),
                "captured_at": meta.get("captured_at", iso_now()),
                "meta": {"captured_by": f"{getpass.getuser()}@{platform.node()} (bless)",
                         "blessed_note": args.note or ""},
            })
        manifest = dict(src_manifest)
    else:
        hints = existing if args.match_existing else None
        extraction = extract_scenes(source, cfg, hints)
        excluded = set(args.exclude or [])
        extraction["scenes"] = [sc for sc in extraction["scenes"] if sc["name"] not in excluded]
        if not extraction["scenes"]:
            raise VisualError(f"bless: no stable scenes in {source}")
        scenes = [{
            "name": s["name"], "im": s["im"], "frame": s["frame"], "present": s["present"],
            "mean_luma": s["mean_luma"], "luma_std": s["luma_std"], "saturation": s["saturation"],
            "black_share": s["black_share"], "bright_share": s["bright_share"],
            "stable_samples": s["stable_samples"], "source_frame": s["source_frame"],
            "captured_at": iso_now(),
            "meta": {"captured_by": f"{getpass.getuser()}@{platform.node()} (bless)",
                     "blessed_note": args.note or ""},
        } for s in extraction["scenes"]]
        manifest = {"label": label, "created": iso_now(), "dynamic": extraction["dynamic"],
                    "source_frames": str(source),
                    "config_path": cfg.get("_config_path"),
                    "config_sha256": cfg.get("_config_sha256")}
    manifest = dict(manifest)
    manifest["label"] = label
    manifest["created"] = manifest.get("created") or iso_now()
    manifest["created_by"] = f"{getpass.getuser()}@{platform.node()}"
    manifest["authorship"] = "bless"
    manifest["blessed_by"] = f"{getpass.getuser()}@{platform.node()}"
    manifest["blessed_at"] = iso_now()
    manifest["note"] = args.note or manifest.get("note", "")
    manifest["platform"] = platform_info()
    print(f"bless: {len(scenes)} scene(s) from {source} -> {label_dir}")
    for s in scenes:
        print(f"  golden {s['name']:<16s} frame {s['frame']} "
              f"luma {_mean(_luma_bytes(s['im'])):.1f}")
    if args.dry_run:
        print("bless: --dry-run: nothing written")
        return 0
    if existing and not args.force:
        print("bless: replacing the existing goldens (pass --force in scripts to make that explicit)")
    write_goldens(label_dir, scenes, manifest)
    return 0


def cmd_verify(args):
    cfg = load_config(args.config)
    label = args.label or default_label()
    label_dir = _common_paths(args, cfg, label)
    store = load_goldens(label_dir, cfg)
    report_dir = Path(args.report_dir) if args.report_dir else REPO / "private" / "bayonetta3" / "visual" / "reports" / label
    report_dir.mkdir(parents=True, exist_ok=True)
    if store is None and not args.adopt_if_missing:
        print(f"visual verify: no goldens for label {label!r} under {label_dir}; skipping "
              f"(bootstrap with: capture --exe <game> --data-root <root> --label {label})")
        payload = {"ran": False, "ok": True, "skipped": True, "label": label,
                   "reason": "no-goldens", "goldens_dir": str(label_dir), "timestamp": iso_now()}
        _write_json(report_dir / "visual-report.json", payload)
        if args.json:
            _write_json(Path(args.json), payload)
        return 0
    if args.frames:
        report = check_frames(Path(args.frames), store, cfg, label, allow_resize=False,
                              report_dir=report_dir, quiet=args.quiet)
        out = {"ran": True, "ok": report["ok"], "label": label, "adopted": False, "capture": None,
               "check": report, "failed_scenes": report["failed_scenes"],
               "failures": report["failures"], "warnings": report["warnings"],
               "goldens_dir": str(label_dir), "timestamp": iso_now()}
        _write_json(report_dir / "visual-report.json", out)
        if args.json:
            _write_json(Path(args.json), out)
        print(report["summary"])
        return 0 if report["ok"] else 1
    capture = _capture_with_retries(args, cfg, label)
    adopted = False
    if store is None and args.adopt_if_missing:
        label_dir, manifest = adopt_capture(capture, cfg, args, label, note="verify --adopt-if-missing")
        print(f"verify: adopted {len(manifest['scenes'])} goldens -> {label_dir}")
        store = load_goldens(label_dir, cfg)
        adopted = True
    report = check_frames(capture["frames_dir"], store, cfg, label, allow_resize=False,
                          report_dir=report_dir, quiet=args.quiet)
    _append_calibration(label_dir, {"label": label, "run": capture["meta"], "timestamp": iso_now(),
                                    "scenes": {s["scene"]: s.get("metrics")
                                               for s in report["scenes"] if s.get("metrics")}})
    out = {
        "ran": True,
        "ok": report["ok"],
        "label": label,
        "adopted": adopted,
        "capture": capture["meta"],
        "check": report,
        "failed_scenes": report["failed_scenes"],
        "failures": report["failures"],
        "warnings": report["warnings"],
        "goldens_dir": str(label_dir),
        "timestamp": iso_now(),
    }
    _write_json(report_dir / "visual-report.json", out)
    if args.json:
        _write_json(Path(args.json), out)
    print(report["summary"])
    return 0 if report["ok"] else 1


def _write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    os.replace(tmp, path)


# --------------------------------------------------------------------------------------------------
# selftest: synthetic images, no game, no network
# --------------------------------------------------------------------------------------------------

def _synth_scene(kind, w=320, h=180):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            if kind == "title":
                base = int(228 - 55 * (y / max(1, h - 1)) + 8 * ((x // 24) % 2))
                px[x, y] = (base, base, min(255, base + 2))
            elif kind == "menu":
                base = 34 if (x < w * 0.7 or y > h * 0.8) else 26
                if 18 < y < h - 26 and (x // 40) % 7 == 0 and (y // 9) % 3 == 0:
                    base = 200
                if x > w - 26 or y > h - 20:
                    base = 8
                px[x, y] = (base, base, base)
            elif kind == "arena":
                checker = 90 + 100 * (((x // 13) + (y // 11)) % 2)
                wave = int(40 * ((x * 5 + y * 3) % 17) / 17)
                px[x, y] = ((checker + wave) % 256, (checker // 2 + 30) % 256, (120 + wave) % 256)
            elif kind == "stripes":
                on = ((x // 2) + (y // 12)) % 2
                px[x, y] = (250, 250, 250) if on else (5, 5, 5)
            elif kind == "plain":
                px[x, y] = (60, 90, 140)
    d = ImageDraw.Draw(im)
    if kind == "title":
        d.rectangle([w // 2 - 40, int(h * 0.72), w // 2 + 40, int(h * 0.76)], fill=(245, 245, 245))
        for k in range(6):
            d.rectangle([40 + k * 40, int(h * 0.9), 60 + k * 40, int(h * 0.92)], fill=(90, 90, 90))
    if kind == "arena":
        d.ellipse([w // 2 - 30, h // 3, w // 2 + 30, h // 3 + 60], fill=(230, 60, 40))
        d.rectangle([20, 20, 70, 50], fill=(240, 210, 50))
    return im


def _noise(im, amount=2, seed=7):
    import random
    rng = random.Random(seed)
    out = im.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b = px[x, y]
            px[x, y] = (max(0, min(255, r + rng.randint(-amount, amount))),
                        max(0, min(255, g + rng.randint(-amount, amount))),
                        max(0, min(255, b + rng.randint(-amount, amount))))
    return out


def _shift(im, dx=1, dy=0):
    out = Image.new("RGB", im.size, (0, 0, 0))
    out.paste(im, (dx, dy))
    return out


def _store_from_images(images, label="selftest", cfg=None):
    scenes = []
    for order, (name, im) in enumerate(images.items()):
        scenes.append({"scene": name, "im": im, "meta": {"resolution": list(im.size)},
                       "feat": analyze_rgb(im), "check_feat": _check_feat(im, cfg or _selftest_cfg()),
                       "file": name + ".png", "order": order})
    return {"manifest": {"label": label, "created": iso_now()}, "scenes": scenes,
            "label_dir": "(selftest)"}


def _selftest_cfg():
    cfg = load_config(None)
    cfg["labels"] = {"selftest": {"required": ["title"], "optional": ["menu", "arena"]}}
    return cfg


def _write_frames(dir_path, items):
    dir_path = Path(dir_path)
    dir_path.mkdir(parents=True, exist_ok=True)
    for p in dir_path.iterdir():
        if p.is_file():
            p.unlink()
    for n, im in items:
        im.save(dir_path / f"{n:05d}.png")


def _expect(name, report, ok, failed_contains=None, warnings_contain=None):
    problems = []
    if report["ok"] != ok:
        problems.append(f"ok={report['ok']} expected {ok}")
    if failed_contains:
        for scene in failed_contains:
            if scene not in report["failed_scenes"]:
                problems.append(f"{scene!r} not in failed {report['failed_scenes']}")
    if warnings_contain:
        text = " ".join(report["warnings"])
        for needle in warnings_contain:
            if needle not in text:
                problems.append(f"{needle!r} not in warnings {report['warnings']}")
    if problems:
        raise Regression(f"{name}: " + "; ".join(problems))


def run_selftest(quiet=False):
    """Synthetic checks for the metrics, matching, thresholds and golden workflow. No game needed."""
    cfg = _selftest_cfg()
    title = _synth_scene("title")
    menu = _synth_scene("menu")
    arena = _synth_scene("arena")
    goldens = _store_from_images({"title": title, "menu": menu, "arena": arena})
    checks = []
    with __import__("tempfile").TemporaryDirectory(prefix="vr-selftest-") as tmp:
        tmp = Path(tmp)
        base = [(10, title), (20, menu), (30, arena)]

        def run(items, allow_resize=False):
            _write_frames(tmp / "frames", items)
            return check_frames(tmp / "frames", goldens, cfg, "selftest",
                                allow_resize=allow_resize, report_dir=tmp / "report", quiet=True)

        # 1. identical frames pass.
        rep = run(base)
        _expect("identical", rep, True)
        checks.append("identical frames pass")
        # 2. small per-pixel noise (the inter-run noise floor) still passes.
        rep = run([(10, _noise(title)), (20, _noise(menu)), (30, _noise(arena))])
        _expect("small noise", rep, True)
        checks.append("small noise passes")
        # 3. one-pixel shift of high-frequency content is a real difference: the checker fails it
        #    (as a diff or as a missing scene) and the structural score collapses.
        stripes = _synth_scene("stripes")
        shifted = _shift(stripes)
        m = compare_metrics(analyze_rgb(stripes, (480, 270)), analyze_rgb(shifted, (480, 270)), cfg)
        if not (m["ssim"] < cfg["check"]["thresholds"]["ssim_min"] or m["luma_mae"] > 6.0):
            raise Regression(f"1px shift: metrics too small (ssim {m['ssim']}, luma {m['luma_mae']})")
        store_s = _store_from_images({"stripes": stripes}, cfg=cfg)
        _write_frames(tmp / "frames", [(10, shifted)])
        rep = check_frames(tmp / "frames", store_s, cfg, "selftest",
                           report_dir=tmp / "report", quiet=True)
        _expect("1px shift", rep, False)
        checks.append("1-pixel shift fails")
        # 4. a black tile where the golden has detail fails.
        broken = arena.copy()
        ImageDraw.Draw(broken).rectangle([4 * 40, 2 * 36, 4 * 40 + 39, 2 * 36 + 35], fill=(0, 0, 0))
        rep = run([(10, title), (20, menu), (30, broken)])
        _expect("black tile", rep, False, failed_contains=["arena"])
        checks.append("black tile fails")
        # 5. desaturated (chroma-collapsed) frame fails.
        rep = run([(10, title.convert("L").convert("RGB")), (20, menu.convert("L").convert("RGB")),
                   (30, arena.convert("L").convert("RGB"))])
        _expect("colour collapse", rep, False)
        checks.append("chroma collapse fails")
        # 6. wrong resolution fails, and passes only with --allow-resize.
        big = [(n, im.resize((640, 360), Image.BILINEAR)) for n, im in base]
        rep = run(big)
        _expect("wrong resolution", rep, False)
        rep = run(big, allow_resize=True)
        _expect("wrong resolution allowed", rep, True)
        checks.append("resolution mismatch fails unless --allow-resize")
        # 7. black frame fails.
        black = Image.new("RGB", title.size, (0, 0, 0))
        rep = run([(10, black), (20, menu), (30, arena)])
        _expect("black frame", rep, False)
        checks.append("black frame fails")
        # 8. a missing required scene fails; a missing optional scene only warns.
        rep = run([(20, menu), (30, arena)])
        _expect("missing required", rep, False, failed_contains=["title"])
        rep = run([(10, title), (20, menu)])
        _expect("missing optional", rep, True, warnings_contain=["arena: missing"])
        checks.append("missing required fails, missing optional warns")
        # 9. an extra stable scene warns without failing.
        extra = _synth_scene("plain")
        rep = run([(10, title), (15, extra), (16, extra), (17, extra), (20, menu), (30, arena)])
        _expect("extra scene", rep, True, warnings_contain=["extra scene"])
        checks.append("extra scene warns, does not fail")
        # 10. threshold math: identical metrics are zero/one, shifted metrics are positive.
        f1, f2 = analyze_rgb(title), analyze_rgb(_shift(title))
        same = compare_metrics(analyze_rgb(title), analyze_rgb(title), cfg)
        if not (same["luma_mae"] == 0 and same["chroma_mae"] == 0 and same["ssim"] > 0.999):
            raise Regression("threshold math: identical images are not a perfect match")
        moved = compare_metrics(f1, f2, cfg)
        if not (moved["luma_mae"] > 0 and moved["tile_luma_mae"] > same["tile_luma_mae"]):
            raise Regression("threshold math: 1-pixel shift produced no difference")
        checks.append("threshold math")
        # 11. golden write/read/bless roundtrip: extract, bless, check passes.
        stable = [(10, title), (11, title), (12, title), (20, menu), (21, menu), (22, menu),
                  (30, arena), (31, arena), (32, arena)]
        _write_frames(tmp / "capture", stable)
        extraction = extract_scenes(tmp / "capture", cfg)
        names = {s["name"] for s in extraction["scenes"]}
        if "title" not in names:
            raise Regression(f"extract: title not found in stable frames (got {names})")
        manifest = {"label": "selftest", "created": iso_now()}
        scenes = [{"name": s["name"], "im": s["im"], "frame": s["frame"],
                   "mean_luma": s["mean_luma"], "luma_std": s["luma_std"],
                   "saturation": s["saturation"], "black_share": s["black_share"],
                   "stable_samples": s["stable_samples"]} for s in extraction["scenes"]]
        label_dir = tmp / "goldens"
        write_goldens(label_dir, scenes, manifest)
        loaded = load_goldens(label_dir, cfg)
        if loaded is None or len(loaded["scenes"]) < 3:
            raise Regression("golden roundtrip: manifest did not load back")
        _write_frames(tmp / "check", base)
        rep = check_frames(tmp / "check", loaded, cfg, "selftest", quiet=True)
        _expect("golden roundtrip", rep, True)
        checks.append("extract -> goldens -> check roundtrip")
        # 12. one frame cannot stand in for two scenes: the second golden is reported missing,
        #     not silently matched to the same frame.
        store2 = _store_from_images({"title": title, "logo": title})
        cfg2 = _selftest_cfg()
        cfg2["labels"] = {"selftest": {"required": [], "optional": ["title", "logo"],
                                      "default_required": False}}
        _write_frames(tmp / "check", [(10, title)])
        rep = check_frames(tmp / "check", store2, cfg2, "selftest", quiet=True)
        if not rep["ok"]:
            raise Regression(f"unique assignment: expected pass, got {rep['failures']}")
        matched = [s for s in rep["scenes"] if s["status"] == "pass"]
        if len(matched) != 1:
            raise Regression(f"unique assignment: one frame matched {len(matched)} scenes")
        checks.append("unique frame-to-scene assignment")

    if not quiet:
        for i, name in enumerate(checks, 1):
            print(f"ok {i:02d} - {name}")
        print(f"SELFTEST: PASS ({len(checks)} checks)")
    return True


# --------------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------------

def _add_common(p, cfg=True, capture=False):
    p.add_argument("--label", default=None,
                   help="platform/driver label of the golden set (default: %s via NX_VISUAL_LABEL)" % default_label())
    p.add_argument("--goldens-root", default=None, help="goldens root (default private/bayonetta3/visual)")
    if cfg:
        p.add_argument("--config", default=None, help="threshold/scene config (default tools/visual_baseline.json)")
    if capture:
        p.add_argument("--exe", default=None, help="game executable (default NX_VISUAL_EXE)")
        p.add_argument("--data-root", default=None, help="NX_DATA_ROOT of the run (default: NX_DATA_ROOT or private/run-bayonetta3)")
        p.add_argument("--frames-dir", default=None, help="(unused; the runtime dumps into <data-root>/frames)")
        p.add_argument("--report-dir", default=None, help="where the report/heatmaps/log go")
        p.add_argument("--seconds", type=float, default=None, help="capture length in seconds (default config)")
        p.add_argument("--every", type=float, default=None, help="seconds between sampled frames (default config)")
        p.add_argument("--quiet", action="store_true", help="no per-frame lines")


def run_selftest_cli(quiet=False):
    """CLI wrapper: 0 when every synthetic check passes, 1 on a real selftest failure.

    run_selftest() returns True on success; the CLI must not pass that to int() (that was F1:
    the `selftest` subcommand exited 1 on success while `--selftest` exited 0).
    """
    try:
        ok = run_selftest(quiet=quiet)
    except Regression as exc:
        print(f"SELFTEST: FAIL ({exc})", file=sys.stderr)
        return 1
    return 0 if ok else 1


def cmd_selftest(args):
    return run_selftest_cli(quiet=args.quiet)


def build_parser():
    ap = argparse.ArgumentParser(
        prog="visual_regress.py",
        description="Per-scene visual baseline regression for Bayonetta 3 (goldens + perceptual diff).",
        epilog="exit codes: 0 pass/skipped, 1 visual regression, 2 usage or configuration problem")
    ap.add_argument("--version", action="version", version=f"{TOOL} {VERSION}")
    sub = ap.add_subparsers(dest="command")

    cap = sub.add_parser("capture", help="drive the game headless and adopt one golden per stable scene")
    _add_common(cap, cfg=True, capture=True)
    cap.add_argument("--adopt", action="store_true", help="adopt the captured scenes even if goldens exist")
    cap.add_argument("--no-adopt", action="store_true", help="keep the frames only; do not write goldens")
    cap.add_argument("--note", default="", help="authorship note stored in the manifest")
    cap.add_argument("--retries", type=int, default=1,
                     help="retries when the run renders no usable scene (default 1)")
    cap.set_defaults(func=cmd_capture)

    chk = sub.add_parser("check", help="compare a frames dir against the label's goldens")
    _add_common(chk, cfg=True, capture=False)
    chk.add_argument("--frames", default=None, help="frames dir (ppm/png) or a single image")
    chk.add_argument("--report-dir", default=None, help="where report.json and diff heatmaps go")
    chk.add_argument("--json", default=None, help="also write the machine-readable report here")
    chk.add_argument("--allow-resize", action="store_true",
                     help="compare a differently sized frame by resizing (default: resolution mismatch fails)")
    chk.add_argument("--quiet", action="store_true", help="only the final VISUAL REGRESS line")
    chk.set_defaults(func=cmd_check)

    ben = sub.add_parser("bless", help="promote a run's frames (or a captured goldens dir) to goldens")
    _add_common(ben, cfg=True, capture=False)
    ben.add_argument("--from", dest="source", required=True, help="frames dir, or a goldens dir with manifest.json")
    ben.add_argument("--note", default="", help="why the goldens changed (stored in the manifest)")
    ben.add_argument("--dry-run", action="store_true", help="show what would be written, write nothing")
    ben.add_argument("--force", action="store_true", help="do not print the replacement warning")
    ben.add_argument("--match-existing", action="store_true",
                     help="name extracted scenes after the existing goldens they match")
    ben.add_argument("--exclude", action="append", default=[],
                     help="scene name to leave out (repeatable, e.g. --exclude scene --exclude dark-card)")
    ben.set_defaults(func=cmd_bless)

    ver = sub.add_parser("verify", help="capture + check in one step (the PS1 / CI entry point)")
    _add_common(ver, cfg=True, capture=True)
    ver.add_argument("--json", default=None, help="write the combined machine-readable report here")
    ver.add_argument("--frames", default=None,
                     help="check this frames dir instead of booting the game (no capture; same exit codes)")
    ver.add_argument("--adopt-if-missing", action="store_true",
                     help="bootstrap goldens from this run when the label has none")
    ver.add_argument("--retries", type=int, default=1,
                     help="retries when the run renders no usable scene (default 1)")
    ver.set_defaults(func=cmd_verify)

    st = sub.add_parser("selftest", help="synthetic checks; no game needed")
    st.add_argument("--quiet", action="store_true")
    st.set_defaults(func=cmd_selftest)

    return ap


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "--selftest":
        ap = argparse.ArgumentParser(prog="visual_regress.py --selftest")
        ap.add_argument("--quiet", action="store_true")
        return run_selftest_cli(quiet=ap.parse_args(argv[1:]).quiet)
    ap = build_parser()
    args = ap.parse_args(argv)
    if not getattr(args, "command", None) and not hasattr(args, "func"):
        ap.print_help()
        return 2
    try:
        return int(args.func(args) or 0)
    except VisualError as exc:
        print(f"visual_regress: error: {exc}", file=sys.stderr)
        if getattr(args, "json", None):
            try:
                _write_json(Path(args.json), {"ran": False, "ok": False, "error": str(exc),
                                              "label": getattr(args, "label", None),
                                              "timestamp": iso_now()})
            except OSError:
                pass
        return 2
    except KeyboardInterrupt:
        print("visual_regress: interrupted", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
