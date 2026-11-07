#!/usr/bin/env python3
"""Put the pak files a run actually reads on local disk, leaving the rest on the share.

WHY THIS EXISTS
  The data root's `extract/romfs` is a link to the share, so every RomFS read is an SMB round trip.
  A 2 MB random read measured 139 ms median on this host. The game asks for a few dozen paks
  during a chapter load, in series, so a load that should take a second takes minutes -- and the
  driver sees a black screen long enough to call it a stall and press PLUS. That is what the notes
  used to call "the level load never completes": it does complete, it is just streamed over SMB.
  The measurements are in docs/CHAPTER1-AUTODRIVE.md §11.10 (2026-10-27).

  Two modes, because the answer depends on free disk:
    ranked (--game-log, repeatable): read game logs, rank the paks by the wall time their reads
      actually took, copy what fits the budget, leave the rest on the share. One run only touches
      the part of the game it reached, so pass one log per run.
    --all: copy the whole RomFS. Use it whenever it fits -- ranking can only pick paks a run has
      already read, so a chapter the logs never reached stays slow. The whole set is ~14.8 GB.

  Either way `extract/romfs` becomes a directory of per-file links (local for the copies, the
  share for everything else), which is what makes a partial set possible. Re-running with a
  bigger budget is safe; `--revert` puts the single share link back.

  It only ever writes inside the data root. The share is read-only to it.

usage:
  tools/b3_romfs_localize.py --all                             # every pak (~14.8 GB)
  tools/b3_romfs_localize.py --game-log logs/mac/A-game.log --game-log logs/mac/B-game.log --budget-gb 6
  tools/b3_romfs_localize.py --all --dry-run                    # show what it would copy
  tools/b3_romfs_localize.py --revert
  tools/b3_romfs_localize.py --status

Exit: 0 ok, 1 nothing to do / could not (e.g. the log has no RomFS reads yet), 2 bad usage.
"""
import argparse
import collections
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATA_ROOT = Path(os.environ.get('B3_MAC_DATA_ROOT') or ROOT / 'private/B3-data/run')
LOCAL_DIR = 'extract-romfs-local'   # the copies; sits next to `extract`, never inside it
EXTRACT = 'extract'
OLD = 'extract-share-only'          # where --revert parks the overlay before putting the link back
SHARE_MARKER = '.share-romfs'       # remembers where the share RomFS was, so --revert works when all-local

SLOW = re.compile(r'fs: slow read Contents:/(\S+\.pkz): (\d+) bytes at \d+ took (\d+) ms')


def share_dirs(data_root: Path):
    """(share_extract, share_romfs) -- the pair the data root was bootstrapped from.

    The bootstrap links `extract -> <share>/data/run/extract`, and that directory holds an `exefs`
    link plus a `romfs` link (`../../romfs`). Both are needed: the runtime reads exefs for the NSO
    and romfs for Contents:. Resolve through the link when it is still one, else through the
    overlay's romfs entries (an overlay root has no single extract target any more).
    """
    link = data_root / EXTRACT
    if link.is_symlink():
        target = Path(os.path.realpath(link))
        if (target / 'romfs').exists():
            return target, Path(os.path.realpath(target / 'romfs'))
    romfs = link / 'romfs'
    if romfs.is_dir():
        for f in romfs.iterdir():
            r = Path(os.path.realpath(f))
            if '/Volumes/' in str(r) or '/mnt/' in str(r):
                # r is <share>/data/romfs/<pak>; the bootstrap's extract is <share>/data/run/extract.
                return r.parent.parent / 'run' / EXTRACT, r.parent
    # Fully localized: no entry points at the share any more, so use the marker the localize run
    # left behind. Without it --status and --revert would be unusable exactly when everything is
    # local, which is the state to revert FROM.
    marker = data_root / LOCAL_DIR / SHARE_MARKER
    if marker.is_file():
        share = Path(marker.read_text().strip())
        if share.is_dir():
            return share.parent / 'run' / EXTRACT, share
    raise SystemExit(f'error: cannot find the share RomFS behind {link} '
                     f'(no link, no share-backed entry, no {LOCAL_DIR}/{SHARE_MARKER})')


def rank(game_logs):
    """(pak -> slow_wall_ms, pak -> reads), summed over one or more game logs.

    Several logs is the useful form: one run only touches the part of the game it reached, so a
    chapter that sits through a load reads a different set than the fight after it. Summing keeps a
    pak that is slow in any run in the ranking instead of letting the last run's set win.
    """
    wall = collections.Counter()
    reads = collections.Counter()
    for log in game_logs:
        for m in SLOW.finditer(log.read_text(errors='ignore')):
            wall[m.group(1)] += int(m.group(3))
            reads[m.group(1)] += 1
    return wall, reads


def tree_bytes(path: Path) -> int:
    """Size of a file, or of a directory tree (romfs/movie is a directory of .webm)."""
    if path.is_file():
        return path.stat().st_size
    return sum(f.stat().st_size for f in path.rglob('*') if f.is_file())


def copy_one(src: Path, dst: Path) -> bool:
    if dst.exists() and tree_bytes(dst) == tree_bytes(src):
        return True
    try:
        if src.is_dir():
            shutil.copytree(src, dst, dirs_exist_ok=True)
        else:
            shutil.copyfile(src, dst)
        return True
    except OSError as e:
        print(f'  ! {src.name}: {e}', file=sys.stderr)
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--data-root', type=Path, default=DEFAULT_DATA_ROOT)
    ap.add_argument('--game-log', type=Path, action='append', default=[],
                    help='a run log to rank the paks by; repeatable, one per run (required to localize)')
    ap.add_argument('--sort', choices=('wall', 'density'), default='density',
                    help='"density" (default) is seconds saved per GB, which wins when the budget is '
                         'tight (2 GB: 72%% of the stalls vs 57%% by wall); "wall" is total seconds')
    ap.add_argument('--budget-gb', type=float, default=3.0,
                    help='stop copying once the local copies reach this many GB (default 3)')
    ap.add_argument('--min-ms', type=int, default=30,
                    help='skip paks whose measured slow-read wall is below this (default 30 ms: one read)')
    ap.add_argument('--all', action='store_true',
                    help='localize every pak in the RomFS instead of the ones a log ranks. Use when the '
                         'whole set fits: ranking can only pick paks a run has already reached, so a '
                         'chapter the logs never got to stays on the share (and stays slow).')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--revert', action='store_true', help='put the single share link back')
    ap.add_argument('--status', action='store_true')
    a = ap.parse_args()

    data_root = a.data_root
    share_extract, share = share_dirs(data_root)
    local = data_root / LOCAL_DIR
    extract = data_root / EXTRACT

    if a.status:
        if extract.is_symlink():
            print(f'extract: single share link -> {os.path.realpath(extract)}')
        elif (extract / 'romfs').is_dir():
            nloc = nnas = 0
            for f in (extract / 'romfs').iterdir():
                r = str(os.path.realpath(f))
                if LOCAL_DIR in r:
                    nloc += 1
                else:
                    nnas += 1
            # tree_bytes, not st_size: romfs/movie is a directory and st_size would count only its inode.
            sz = (sum(tree_bytes(f) for f in local.iterdir() if f.name != SHARE_MARKER)
                  if local.is_dir() else 0)
            print(f'extract: overlay  local paks {nloc} ({sz / 1e9:.2f} GB), from the share {nnas}')
        else:
            print(f'extract: {extract} is missing')
        print(f'share RomFS: {share}')
        return 0

    if a.revert:
        if extract.is_symlink():
            print('already a single share link, nothing to do')
            return 0
        park = data_root / OLD
        if park.exists():
            shutil.rmtree(park)
        extract.rename(park)
        extract.symlink_to(share_extract)
        print(f'reverted: {extract} -> {os.path.realpath(extract)}  (overlay parked at {park})')
        return 0

    if not a.all and not a.game_log:
        print('error: --game-log is required to rank the paks (or --all to take the whole RomFS)',
              file=sys.stderr)
        return 2
    for log in a.game_log:
        if not log.exists():
            print(f'error: no such log: {log}', file=sys.stderr)
            return 1

    if a.all:
        # No measurements to rank by, and none needed: every pak is taken, so the order only decides
        # what gets copied first. Largest first keeps the early progress visible.
        every = sorted(share.iterdir(), key=lambda f: -tree_bytes(f))
        wall = collections.Counter({f.name: 0 for f in every})
        reads = collections.Counter({f.name: 0 for f in every})
        total_gb = sum(tree_bytes(f) for f in every) / 1e9
        print(f'--all: {len(every)} paks, {total_gb:.2f} GB to copy')
        if not a.dry_run:
            free = shutil.disk_usage(data_root).free / 1e9
            have = sum(tree_bytes(f) for f in local.iterdir() if f.name != SHARE_MARKER) / 1e9 \
                   if local.is_dir() else 0.0
            if total_gb - have > free:
                print(f'error: would need {total_gb - have:.2f} GB more, {free:.2f} GB free',
                      file=sys.stderr)
                return 1
    else:
        wall, reads = rank(a.game_log)
        if wall:
            print(f'{len(wall)} paks, {sum(wall.values()) / 1000:.1f} s of measured slow-read wall '
                  f'({", ".join(l.name for l in a.game_log)})')
    if not wall:
        print(f'error: no "fs: slow read Contents:/*.pkz" lines in {len(a.game_log)} log(s).\n'
              f'       RomFS reads only show up when they take >= 30 ms, which is exactly the case\n'
              f'       this script exists for -- did the run use a data root whose romfs is local?',
              file=sys.stderr)
        return 1

    total = sum(wall.values())
    # Rank by what the paks are worth against the budget. `density` (seconds saved per GB) beats plain
    # `wall` whenever the read set does not fit -- the big paks are chapters not yet reached, and
    # spending 600 MB on one of those crowds out a dozen small ones that are read every load.
    sized = {}
    for p in wall:
        src = share / p
        sized[p] = tree_bytes(src) if src.exists() else 0
    if a.all:
        order = [p for p in wall]              # already largest first
        # --all means every pak, so the default budget must not stop it part way.
        a.budget_gb = float('inf')
    elif a.sort == 'density':
        order = sorted((p for p in wall if wall[p] >= a.min_ms),
                       key=lambda p: -(wall[p] / max(sized[p], 1)))
    else:
        order = [p for p, w in wall.most_common() if w >= a.min_ms]
    print(f'  {"pak":22s} {"slow wall":>10s} {"reads":>6s}   {"size":>8s}')
    picked, picked_gb = [], 0.0
    sizes = {}
    for p in order:
        src = share / p
        sz = sized[p]
        sizes[p] = sz
        take = src.exists() and (picked_gb + sz / 1e9) <= a.budget_gb
        if take:
            picked.append(p)
            picked_gb += sz / 1e9
        print(f'  {p:22s} {wall[p] / 1000:8.1f} s {reads[p]:6d}   {sz / 1e6:7.0f} M  {"local" if take else ""}')
    if not picked:
        print('nothing fits the budget; raise --budget-gb', file=sys.stderr)
        return 1

    covered = sum(wall[p] for p in picked)
    if total:
        print(f'\n{picked_gb:.2f} GB local covers {covered / 1000:.1f} s of {total / 1000:.1f} s '
              f'({100 * covered / total:.0f}%) of the measured stalls')
    else:
        print(f'\n{picked_gb:.2f} GB local: every pak, nothing left on the share')
    if a.dry_run:
        return 0

    local.mkdir(parents=True, exist_ok=True)
    # Remember where the share RomFS was: a fully localized root has no link left to resolve through,
    # and --status/--revert must still work in exactly that state.
    (local / SHARE_MARKER).write_text(str(share) + '\n')
    print(f'\ncopying {len(picked)} paks to {local} ...')
    for p in picked:
        copy_one(share / p, local / p)
    have = sum(tree_bytes(f) for f in local.iterdir() if f.name != SHARE_MARKER) if local.is_dir() else 0
    print(f'  {local}: {len([f for f in local.iterdir() if f.name != SHARE_MARKER])} files, {have / 1e9:.2f} GB')

    # Rebuild `extract` as a directory of per-file links. Keep exefs as its own link.
    fresh = data_root / (EXTRACT + '-new')
    if fresh.exists():
        shutil.rmtree(fresh)
    (fresh / 'romfs').mkdir(parents=True)
    exefs_src = share_extract / 'exefs'
    if exefs_src.exists():
        (fresh / 'exefs').symlink_to(exefs_src)
    nloc = 0
    for f in sorted(share.iterdir()):
        dst = fresh / 'romfs' / f.name
        if (local / f.name).exists():
            dst.symlink_to(local / f.name)
            nloc += 1
        else:
            dst.symlink_to(f)
    if extract.exists() and not extract.is_symlink():
        park = data_root / OLD
        if park.exists():
            shutil.rmtree(park)
        extract.rename(park)
    else:
        extract.unlink()
    fresh.rename(extract)
    print(f'  {extract}: overlay with {nloc} local / {len(list(share.iterdir())) - nloc} from the share')
    return 0


if __name__ == '__main__':
    sys.exit(main())
