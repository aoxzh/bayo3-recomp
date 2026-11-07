#!/usr/bin/env python3
"""Drives the game past screens it would otherwise stall on, using what it renders.

A blind timed script (NX_INPUT_SCRIPT) presses buttons at fixed seconds: it walks whatever screen
happens to be up, so a slower frame, a longer load or an unexpected prompt desynchronises it and
the run sits on a menu until the timeout. This driver instead looks at the presented frame and
presses what that screen wants, so a run keeps going to the next scene on its own.

It works with the runtime's on-demand frame dump: dropping a file named `dump_frames` into the
data root makes the next frames be written to <data root>/frames/NNNNN.ppm. The driver asks for one
frame every --every seconds, classifies it, and writes the button to hold into the NX_INPUT_LIVE
file, which the runtime polls while it runs.

usage:
  b3_autodrive.py <data root> [--live <file>] [--every 2.0] [--timeout 900] [--log <file>]

Prints one line per decision, so the run can be read back afterwards.
"""
import argparse
import glob
import os
import sys
import time

# What a frame looks like, from its own pixels. The game's screens differ enough in brightness and
# colour spread for these to be separable without reading game memory:
#   black      - a fade or a load: no spread, nothing to press
#   dark-ui    - a dark screen with content on it (auto-save notice, subtitles): A Confirm works
#   bright     - a white flash / logo
#   dark-scene - a cutscene or a dark area (low mean, some bright pixels)
#   scene      - gameplay: a wide range of colours
#   menu       - a menu or a title: mid brightness, flat background, text-like bright pixels
BUTTONS = ("A", "B", "X", "Y", "PLUS", "MINUS", "UP", "DOWN", "LEFT", "RIGHT", "ZL", "ZR", "L", "R")


# Compact English UI glyph masks from the saved-frame regressions. Match both precision and
# recall so a bright flash or a dark picture cannot match just by filling the glyph's pixels.
_CLOSE_ROWS = (
    0x000000000000000, 0x000000000000000, 0x000000000000180, 0x000000000000fe0,
    0x000000010201e70, 0x000000010101838, 0x0000000101039b8, 0x001000010083838,
    0x001109090083838, 0x000121090103bb8, 0x000040090101838, 0x000020011401c30,
    0x000000000000fe0, 0x0000000000003c0, 0x000000000000000, 0x000000000000000,
    0x000000000000000, 0x000000000000000, 0x000000000000000, 0x000000000000000,
)
_DEATH_ROWS = (
    0x000000000000000, 0x000000000000000, 0x000000000000000, 0x000000000000000,
    0x3c22c1f480b80cc, 0x000010848000000, 0x000010848000000, 0x2402d88ce6000b0,
    0x3102d908e603080, 0x016058008003000, 0x002018020001000, 0x1c07b9010180180,
    0x000000000000000, 0x000000000000000, 0x000000000000000, 0x000000000000000,
)


_VERSE_ROWS = (
    0, 0, 0, 0, 7881299347898368, 7318349394477056, 2251808404148752,
    6755399517077504, 6755399517077536, 6755408838918144, 6755400515321856,
    2251800887951424, 2251808412536960, 2251799813685248, 6755399441055744,
    0, 0, 0, 0, 0,
)
_PUNCH_ROWS = (
    0, 0, 0, 0, 0, 48, 248, 422212465074356, 70368744185996,
    949978046407116, 949978046407118, 1020346790584780, 1055531162673316,
    281474976719100, 120, 0, 0, 0, 0, 0,
)


def _prompt_matches(image, box, rows, light, minimum=0.82):
    width, height = image.size
    bounds = tuple(round(value * size / original) for value, size, original in
                   zip(box, (width, height, width, height), (1600, 900, 1600, 900)))
    patch = image.crop(bounds).convert("L").resize((60, len(rows)))
    actual = [value > 160 if light else value < 120 for value in patch.getdata()]
    expected = [bool(row & (1 << x)) for row in rows for x in range(60)]
    overlap = sum(a and b for a, b in zip(actual, expected))
    return overlap / max(1, sum(actual)) >= minimum and overlap / sum(expected) >= minimum


def visible_prompt(path):
    """Recognize tutorial/death UI or the combined health display and combat HUD.

    The tutorial requires both purple panel borders. Combat requires the purple health orb
    plus either the Verse 1 text or the opening X Punch legend. Neither color nor a generic
    moving scene alone can override the movie hold. Coordinates scale with the dumped frame.
    """
    from PIL import Image

    with Image.open(path) as image:
        if _prompt_matches(image, (1000, 785, 1120, 825), _CLOSE_ROWS, light=True):
            small = image.convert("RGB").resize((160, 90))
            borders = []
            for ys in ((12, 13), (76, 77)):
                borders.append(max(sum(small.getpixel((x, y))[2] -
                                       max(small.getpixel((x, y))[:2]) > 15
                                       for x in range(16, 144)) / 128 for y in ys))
            if min(borders) >= 0.75:
                return "tutorial-close"
        if _prompt_matches(image, (440, 400, 1160, 485), _DEATH_ROWS, light=False):
            return "death"
        width, height = image.size
        bounds = tuple(round(value * size / original) for value, size, original in
                       zip((60, 15, 125, 80), (width, height, width, height), (1600, 900, 1600, 900)))
        orb = image.crop(bounds).convert("RGB").resize((20, 20))
        purple = sum(r > 70 and b > 90 and b > g * 1.25 and r > g * 1.15
                     for r, g, b in orb.getdata())
        # The thin Verse glyphs lose strokes when frames are scaled. The separate health
        # region is mandatory; saved combat/intro/menu/flash regressions cover both gates.
        if purple >= 80 and (
                _prompt_matches(image, (1370, 90, 1520, 135), _VERSE_ROWS, light=True, minimum=0.65)
                or _prompt_matches(image, (60, 355, 220, 393), _PUNCH_ROWS, light=True)):
            return "combat"
    return None


def luminance(path):
    """(stats, 160x90 luma grid) of a PPM frame; the grid feeds the motion metric."""
    from PIL import Image

    im = Image.open(path).convert("RGB").resize((160, 90))
    px = list(im.getdata())
    n = len(px)
    lum = [0.299 * r + 0.587 * g + 0.114 * b for r, g, b in px]
    mean = sum(lum) / n
    spread = (sum((v - mean) ** 2 for v in lum) / n) ** 0.5
    bright = sum(1 for v in lum if v > 200) / n
    black = sum(1 for v in lum if v < 12) / n
    return (mean, spread, bright, black), lum


def classify(path):
    """(kind, mean, spread, bright_share, motion) of a PPM frame against the previous one.

    A dark *UI* screen is not a fade: the auto-save notice is mean luma 2.9 with text on it
    (spread 12.3, bright 0.001), and it waits for A Confirm. Classifying it "black" made the
    driver press PLUS instead and sit on it for 72 s (av-audit.md F3). A real fade or a black
    cutscene has almost no spread; that is what separates them.
    """
    (mean, spread, bright, black), lum = luminance(path)
    prompt = visible_prompt(path)
    if prompt:
        kind = prompt
    elif mean < 10 and spread < 8:
        kind = "black"
    elif black > 0.85 and mean < 8 and spread < 8:
        kind = "black"
    elif mean < 12 and spread >= 8:
        kind = "dark-ui"
    elif mean > 200:
        kind = "bright"
    elif mean < 40 and spread > 25:
        kind = "dark-scene"
    elif spread < 22:
        kind = "menu"
    else:
        kind = "scene"
    return kind, mean, spread, bright, lum


def bright_static_menu(kind, mean, motion):
    """A bright, perfectly still menu page -- the in-fight pause menu a movie skip can leave open.

    A7 (2026-10-27) ended up on it for 77 samples: `menu (mean 117.4, motion 0.0)`. The driver's dark
    menu branch only matches 14 < mean < 36, so nothing handled it and the A/B cycle just poked at
    it. The name matters: a bright *moving* UI (a HUD, an animated menu) is not this, and the title
    screen is mean < 10, so neither is matched.
    """
    return kind == "menu" and mean > 60 and motion < 2.0


def latest_frame(frames_dir, since, floor=0):
    """The newest dumped frame with a number above `floor`, or None.

    Keyed on the frame number in the name, not the mtime: a second run over the same data root keeps
    writing its own numbering, and mtime order then mixes frames of both runs.
    """
    best = None
    for p in glob.glob(os.path.join(frames_dir, "[0-9][0-9][0-9][0-9][0-9].ppm")):
        try:
            n = int(os.path.basename(p)[:-4])
        except ValueError:
            continue
        if n <= floor:
            continue
        if best is None or n > best[0]:
            best = (n, p)
    return best


def newest_frame(frames_dir):
    """The newest dumped frame of any number (a fresh runtime numbers from zero again)."""
    return latest_frame(frames_dir, 0, -1)


def write_live(path, button, hold_note=""):
    """Holds `button` now (the runtime polls the file). An empty button releases everything."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        if button:
            f.write(f"0:{button}:0;")
    os.replace(tmp, path)


# A movie is "still playing" if its `movie:` fill count changed within this window. ev0025 prints a
# fill line every 300 decoded frames, which at its 6.5 fps is ~47 s, so the window has to be minutes.
MOVIE_GAP = 120.0
MOVIE_SKIP_TRIES = 6    # skips per movie
MOVIE_SKIP_RETRY = 12.0 # seconds without the skip path in the log before the next try

# How much of the game log tail the movie readers look at. 40 KB was not enough: the log grows much
# faster than a fill line interval (the runtime's pass/wait logging runs at thousands of lines a
# second in the heavy states), so in the 2026-10-27 Deck logs the last `luma plane fill` line was
# 0.2-4.4 MB from the end and the readers saw no movie at all. All four readers share this window.
MOVIE_TAIL_BYTES = 4 * 1024 * 1024

# How many walk+A rounds the static-scene branch may spend on one unmoving picture before it gives up.
# It is a still *menu* as often as a still cutscene (the bar hub's save-point wall reads mean 73,
# motion 0.1), and that branch re-arms its own retrigger, so without a limit it never reaches the
# stalled-shot escape: mac-battle-probe (2026-10-27) spent 18.5 min and 220 rounds on the wall.
STUCK_LIMIT = 6

FIGHT_PATTERN = ("X", "X", "A", "ZR", "X", "ZR", "A", "Y", "UP", "X",
                 "X", "B", "A", "ZR", "X", "A", "RIGHT", "X", "Y", "ZR")
FIGHT_BURST_SECONDS = 5.0


def movie_line_count(path):
    """How many `movie:` lines the game's log has now (-1 when it cannot be read).

    The log has no timestamps, but these lines are written once per decoded movie frame, so a
    growing count means a movie is playing *right now* — the one signal the presented frames do
    not carry (ev0025's static shot looks exactly like a stalled scene).
    """
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - MOVIE_TAIL_BYTES))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return -1
    return tail.count("movie: ")


def movie_fill_count(path, movie=""):
    """How many `luma plane fill` lines the log has for `movie` (all movies when empty), -1 unreadable.

    Scoped to one movie: the global count also grows while the *previous* film is still finishing, so a
    name change followed by an increase from the old film started the skip timer for the new one (it
    then fired at the new film's first frame).

    This is when the movie is really *in front of the player*, as opposed to configured: the decoder
    opens the file and the `fs: movie read ...`/`-> ffmpeg` lines can appear tens of seconds before
    the first frame is presented (ev0005 on the Deck: 40+ s), so a skip timer started from the name
    fired 16 s into the 48.5 s movie and cost 23 of its 1457 frames. A 300-frame fill line at 30 fps
    arrives every 10 s, which is granular enough for "has playback been going for N seconds".
    """
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - MOVIE_TAIL_BYTES))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return -1
    key = os.path.basename(movie) + " luma plane fill" if movie else "luma plane fill"
    return tail.count(key)


def last_movie_file(path):
    """The `movie/<name>.webm` the log last mentioned ('' when unknown).

    ev0023 -> ev0025 come with no >MOVIE_GAP silence between them, so the movie-start reset has to
    key on the *name*, or the skip attempt is only ever spent on the first movie of the chain.
    """
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - MOVIE_TAIL_BYTES))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return ""
    i = tail.rfind("movie/")
    if i < 0:
        return ""
    j = tail.find(".webm", i)
    return tail[i:j + 5] if j >= 0 else ""


def last_fill_file(path):
    """The `movie/<name>.webm` of the log's last `luma plane fill` line ('' when unknown).

    The skip timer must follow *playback*, and the fill line is the playback: `luma plane fill #N`
    is written while that film is on screen. `last_movie_file` cannot anchor it. When the next film
    is prepared, its reader/`replacing ...`/`source open` lines appear in the log while the current
    film still plays, so the last `movie:` mention flips between two names every few lines. Keying
    the timer on that name reset it each time, the timer never reached --movie-skip-after, and the
    six-minute films played out in full (2026-10-27 Deck: 500 s of drive went into the movie chain
    and the Prologue fight was never reached). The fill lines do not interleave: each film's fills
    come as one run (ev0021 x17, ev0023 x13, ev0025 x19 in the same log).
    """
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - MOVIE_TAIL_BYTES))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return ""
    i = tail.rfind("luma plane fill")
    if i < 0:
        return ""
    line = tail[tail.rfind("\n", 0, i) + 1:i]
    j = line.rfind("movie/")
    if j < 0:
        return ""
    k = line.find(".webm", j)
    return line[j:k + 5] if k >= 0 else ""


def skip_path_count(path):
    """How many times the game has taken its skip path. The driver's only proof a movie skip landed:
    a press can open the pause menu and not reach Skip, and then the film just keeps playing."""
    try:
        with open(path, errors="replace") as f:
            return sum("(skip path) arg0 1" in line for line in f)
    except OSError:
        return 0


def keep_movie_frame(path, movie_name, root):
    """Copy a dumped frame to <root>/frames-movie/<movie>/ and return the destination ('' on failure).

    While a movie plays the driver presses nothing, and the runtime only writes a frame when asked, so
    the acceptance question "is the picture on screen the movie's own" has no evidence unless the
    samples are lifted out of frames/: that directory keeps only the newest 80 files, so the movie's
    dumps are pruned as soon as play resumes.
    """
    name = os.path.basename(movie_name or "") or "unknown"
    movie = os.path.splitext(name)[0]
    out = os.path.join(root, "frames-movie", movie)
    try:
        os.makedirs(out, exist_ok=True)
        dst = os.path.join(out, os.path.basename(path))
        with open(path, "rb") as src, open(dst, "wb") as f:
            f.write(src.read())
    except OSError:
        return ""
    return dst


def fight(live, seconds, hold=0.14, gap=0.06):
    """Attack/evade/jump pattern, one button held at a time, for at most `seconds`.

    The preserved control HUD explicitly maps X=Punch, A=Kick, Y=Shoot, B=Jump, ZR=Dodge.
    Each 20-input cycle uses 7 punches, 4 kicks, 2 shots, 4 evades, 1 jump and 2 directional steps.
    Keep the pattern fixed across comparison arms. Short bursts let the caller inspect UI.
    """
    deadline = time.time() + seconds
    i = 0
    try:
        while time.time() < deadline:
            write_live(live, FIGHT_PATTERN[i % len(FIGHT_PATTERN)])
            time.sleep(min(hold, max(0.0, deadline - time.time())))
            write_live(live, "")
            time.sleep(min(gap, max(0.0, deadline - time.time())))
            i += 1
    finally:
        write_live(live, "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_root")
    ap.add_argument("--live", default=None, help="NX_INPUT_LIVE file (default <data root>/input_live.txt)")
    ap.add_argument("--every", type=float, default=2.0, help="seconds between frames looked at")
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--log", default=None)
    ap.add_argument("--settle", type=float, default=0.35, help="how long a press is held")
    ap.add_argument("--black-patience", type=int, default=15,
                    help="consecutive black samples before it counts as a stall. A cutscene's black "
                         "cuts last seconds, not a quarter minute; the old default of 3 pressed PLUS "
                         "inside every movie black-out, which toggled the pause menu on ev0021")
    ap.add_argument("--game-log", default=None,
                    help="the game's log; while `movie:` lines grow in it a movie is playing, and the "
                         "driver waits instead of pressing (the pause-menu cycle inside ev0025)")
    ap.add_argument("--a-only-until", type=float, default=0.0,
                    help="seconds of A-only driving at the start. Paired with NX_INPUT_SCRIPT's boot "
                         "sequence (title -> Continue -> the story's movie chain): without it the "
                         "driver's A/B/DOWN cycle walks into OPTIONS from the main menu")
    ap.add_argument("--movie-frames", type=float, default=3.0,
                    help="seconds between picture samples taken *while a movie plays* (0 disables). "
                         "They are copied to <data root>/frames-movie/<movie>/ because the runtime only "
                         "dumps on demand and no button is pressed during playback: this is the "
                         "evidence for 'is the cutscene's own picture on screen'. The 80-file prune in "
                         "frames/ would otherwise delete the movie's dumps as soon as play resumes")
    ap.add_argument("--movie-skip-after", type=float,
                    default=float(os.environ.get("B3_MOVIE_SKIP_AFTER", "150")),
                    help="seconds of *playback* (first plane fill, not the filename appearing in the "
                         "log) before a movie gets one PLUS/DOWN/A skip attempt. A movie's wall time is "
                         "its frame count over the game's frame rate, not its media time: ev0005 is "
                         "48.5 s of video but ~90 s of wall time in the chapter's slow state, so both "
                         "60 s (from the name) and 60 s (from the first fill) cut it short (1435 and "
                         "~1200 of 1457 fills). 150 s still skips ev0025's six-minute static shot. "
                         "B3_MOVIE_SKIP_AFTER sets the default (the battle benchmark uses 60 s to get "
                         "past the Prologue's cutscene chain); the flag always wins")
    ap.add_argument("--take-yes", action="store_true",
                    help="take the YES option (LEFT then A) on a YES/No page. For the completed-save "
                         "hub, whose chapter-start confirm needs it (m3-save3). Off by default: on the "
                         "intro/Prologue pages the highlighted No is the safe answer, and forcing Yes "
                         "confirmed a return-to-title-class page and replayed the intro (2026-10-26)")
    a = ap.parse_args()

    root = a.data_root
    live = a.live or os.path.join(root, "input_live.txt")
    frames_dir = os.path.join(root, "frames")
    dump_marker = os.path.join(root, "dump_frames")
    os.makedirs(frames_dir, exist_ok=True)
    log = open(a.log, "w") if a.log else None

    def say(msg):
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line, flush=True)
        if log:
            log.write(line + "\n")
            log.flush()

    say(f"autodrive: data root {root}, live file {live}")
    last_movie_lines = -1
    last_movie_time = 0.0
    playback_started = 0.0
    last_fill_lines = -1
    movie_skip_last = 0.0  # when the last skip of this movie was sent
    movie_skip_tries = 0
    movie_skip_seen = 0    # skip_path_count when it was sent
    last_movie_name = ""
    last_movie_sample = 0.0
    started = time.time()
    last_frame_no = 0
    presses = 0
    same_kind = 0
    static_rounds = 0  # walk+A rounds spent on the current unmoving picture (STUCK_LIMIT)
    last_kind = None
    prev_lum = None
    black_streak = 0
    pause_pending = False  # a PLUS was sent and the pause menu has not answered yet
    battle_votes = 0
    skip_at = 0.0          # when a movie skip fired: the bright-menu escape only runs near it
    escape_streak = 0      # consecutive bright static menu samples after that skip
    # A frame left by an earlier run is not this run's: the runtime numbers from zero again, and a
    # floor above its numbering would filter every new frame out for good.
    if (old := newest_frame(frames_dir)) and not os.path.exists(os.path.join(root, "autodrive_run")):
        say(f"clearing {old[1]} and its run's frames (an earlier run's numbering)")
        for p in glob.glob(os.path.join(frames_dir, "*.ppm")) + glob.glob(os.path.join(frames_dir, "*.raw")):
            try:
                os.remove(p)
            except OSError:
                pass
    with open(os.path.join(root, "autodrive_run"), "w") as f:
        f.write(f"{time.time()}\n")
    try:
        while time.time() - started < a.timeout:
            observed = None  # (frame, classification), reused when visible UI overrides movie hold
            # Boot phase: only A, so the main menu's default entry (STORY/Continue) is taken instead
            # of walking the options list with the A/B/DOWN cycle. The movie chain is A-driven too.
            if time.time() - started < a.a_only_until:
                write_live(live, "A")
                presses += 1
                time.sleep(a.settle)
                write_live(live, "")
                time.sleep(max(0.0, a.every - a.settle))
                continue
            # A movie is playing: no button can help and the pause-menu cycle only breaks playback.
            # (ev0025 runs six minutes, most of it a static shot; pressing PLUS inside it opened and
            # closed the pause menu for the rest of the run in m1-run2/3.)
            if a.game_log:
                n = movie_line_count(a.game_log)
                if n > 0 and n != last_movie_lines:   # zero movie lines = no movie at all
                    # Anchor on the film that is *playing* (its luma-plane-fill line), not on the last
                    # `movie:` mention: the next film's reader/replacing lines interleave with the
                    # current film's playback, so the last-mention name flips every few lines and the
                    # skip timer would restart before it ever reached --movie-skip-after.
                    name = last_fill_file(a.game_log) or last_movie_file(a.game_log)
                    if (name and name != last_movie_name) or last_movie_lines < 0 \
                            or time.time() - last_movie_time > MOVIE_GAP:
                        # A new movie. The skip timer below is started from the first plane fill, not
                        # from here: the filename can be in the log (fs: read lines) long before the
                        # player shows the picture, and the skip must not land inside playback.
                        playback_started = 0.0
                        movie_skip_last = 0.0
                        movie_skip_tries = 0
                        last_movie_name = name
                        last_movie_lines = n
                        last_movie_time = time.time()
                        last_fill_lines = -1   # baseline the fill count for this movie below
                    else:
                        last_movie_lines = n
                        last_movie_time = time.time()
                # Playback has really started when the *fill* count for this movie goes up. A baseline
                # is taken first: the tail window slides as the log grows, which can make the count go
                # down, and only a count that grows means a new plane fill line was written.
                fills = movie_fill_count(a.game_log, last_movie_name)
                if fills >= 0:
                    if last_fill_lines < 0:
                        last_fill_lines = fills
                    elif fills > last_fill_lines:
                        last_fill_lines = fills
                        if not playback_started:
                            playback_started = time.time()   # the long ones are worth skipping from here
                if time.time() - last_movie_time < MOVIE_GAP:
                    write_live(live, "")
                    now = time.time()
                    if a.movie_frames > 0 and now - last_movie_sample >= a.movie_frames:
                        last_movie_sample = now
                        with open(dump_marker, "w") as f:
                            f.write("1 1")
                        deadline = time.time() + max(3.0, a.every * 2)
                        got = None
                        while time.time() < deadline:
                            time.sleep(0.2)
                            got = latest_frame(frames_dir, 0, last_frame_no)
                            if got:
                                break
                        if got:
                            last_frame_no, path = got
                            kept = keep_movie_frame(path, last_movie_name, root)
                            if kept:
                                say(f"movie frame kept: {os.path.basename(path)} -> {kept}")
                            try:
                                decision = classify(path)
                            except Exception as exc:
                                say(f"could not inspect movie frame {os.path.basename(path)}: {exc}")
                            else:
                                if decision[0] in ("combat", "tutorial-close", "death"):
                                    observed = (got, decision)
                                    say(f"visible {decision[0]} overrides movie hold: {os.path.basename(path)}")
                    if observed is None:
                        # Keep true movies un-driven even when a bounded log tail's count falls
                        # or saturates. Visible HUD/prompt evidence, not that count, overrides it.
                        # One failed skip used to lose the run: the film played on into ev0022 and ev0023 and
                        # the fight never came. Retry until the game's skip path shows up in its log.
                        skip_due = playback_started and time.time() - playback_started > a.movie_skip_after and (
                            movie_skip_tries == 0 or (
                                movie_skip_tries < MOVIE_SKIP_TRIES
                                and time.time() - movie_skip_last >= MOVIE_SKIP_RETRY
                                and skip_path_count(a.game_log) == movie_skip_seen))
                        if skip_due:
                            movie_skip_seen = skip_path_count(a.game_log)
                            say(f"movie {last_movie_name or '?'} playing for "
                                f"{time.time() - playback_started:.0f} s (--movie-skip-after "
                                f"{a.movie_skip_after:.0f}): PLUS, DOWN, A to skip it, try {movie_skip_tries + 1}")
                            # Short presses with long waits: the pause menu slides in and takes no input until it
                            # has. The old 1.2 s hold and 0.6 s gap failed six tries running at 15 W and at 29 W alike
                            # (vx15a, vx29a); this timing took the first try in all three runs that used it.
                            for button, hold, wait in (("PLUS", 0.3, 3.0), ("DOWN", 0.2, 1.0), ("A", 0.2, 2.0), ("A", 0.2, 0.0)):
                                write_live(live, button)
                                time.sleep(hold)
                                write_live(live, "")
                                time.sleep(wait)
                            presses += 4
                            movie_skip_tries += 1
                            movie_skip_last = time.time()
                            skip_at = time.time()
                        time.sleep(a.every)
                        continue
            if observed is None:
                # Ask for a frame, wait for the runtime to write it.
                with open(dump_marker, "w") as f:
                    f.write("1 1")
                deadline = time.time() + max(3.0, a.every * 3)
                got = None
                while time.time() < deadline:
                    time.sleep(0.2)
                    got = latest_frame(frames_dir, 0, last_frame_no)
                    if got:
                        break
                if not got:
                    # A restarted runtime numbers from zero again.
                    if (fresh := newest_frame(frames_dir)) and fresh[0] < last_frame_no:
                        say(f"the runtime restarted (frame {fresh[0]} after {last_frame_no}): following it")
                        last_frame_no = 0
                        got = fresh
                if not got:
                    say("no new frame dumped (the runtime may have stopped); releasing buttons")
                    write_live(live, "")
                    time.sleep(a.every)
                    continue
                try:
                    decision = classify(got[1])
                except Exception as exc:
                    say(f"could not read {os.path.basename(got[1])}: {exc}")
                    time.sleep(a.every)
                    continue
            else:
                got, decision = observed
            last_frame_no, path = got
            kind, mean, spread, bright, lum = decision
            motion = 0.0
            if prev_lum is not None and len(prev_lum) == len(lum):
                motion = sum(abs(x - y) for x, y in zip(lum, prev_lum)) / len(lum)
            prev_lum = lum
            black_share = sum(1 for v in lum if v < 12) / len(lum)
            # Keep the disk bounded: a 900 s drive dumps ~900 4.3 MB frames. The newest 80 *files*
            # are kept (one dump every --every seconds); keying this on the frame number deleted
            # every older dump, because consecutive dumps are ~120 presents apart.
            fs = sorted(glob.glob(os.path.join(frames_dir, "[0-9][0-9][0-9][0-9][0-9].ppm")))
            for p_old in fs[:-80]:
                try:
                    os.remove(p_old)
                except OSError:
                    pass
            same_kind = same_kind + 1 if kind == last_kind else 1
            if kind != last_kind:
                static_rounds = 0  # a different picture gets its own STUCK_LIMIT budget
            last_kind = kind
            say(f"{os.path.basename(path)}: {kind} (mean {mean:.1f}, spread {spread:.1f}, bright {bright:.3f}, "
                f"motion {motion:.1f}), same for {same_kind}")

            # These are visible UI prompts, not combat. In candidate-a the Clouds of Erasure
            # card looked like a static scene (mean 32.0, spread 22.7), so the walk/A recovery
            # reset same_kind forever and never reached the fallback's B press.
            if kind in ("tutorial-close", "death"):
                if kind == "tutorial-close":
                    say("TUTORIAL: visible B Close prompt; B to close")
                    button = "B"
                else:
                    say("DEATH: The Witch Hunts Are Over; A to acknowledge and await retry")
                    button = "A"
                write_live(live, button)
                presses += 1
                time.sleep(a.settle)
                write_live(live, "")
                battle_votes = 0
                same_kind = 0
                pause_pending = False
                time.sleep(max(0.0, a.every - a.settle))
                continue

            if kind == "combat":
                say(f"BATTLE: visible health and combat HUD; fighting for {FIGHT_BURST_SECONDS:g}s")
                fight(live, FIGHT_BURST_SECONDS)
                battle_votes = 0
                same_kind = 0
                pause_pending = False
                time.sleep(a.every)
                continue

            # A skip that lands on a pause menu can leave it open after the film ends: the picture is
            # a bright, perfectly still menu page (see bright_static_menu()). Only inside 120 s of a
            # fired skip, so a title/options screen can never be poked by this: B backs out of a
            # menu, and if the same signature survives 3 samples it is the pause screen, which PLUS
            # toggles closed. Every attempt is logged with the mean/motion so the drive log shows it.
            if skip_at and time.time() - skip_at < 120 and bright_static_menu(kind, mean, motion):
                escape_streak += 1
                if escape_streak >= 3:
                    say(f"bright static menu after the skip (mean {mean:.1f}, motion {motion:.1f}): "
                        f"PLUS to toggle the pause menu")
                    write_live(live, "PLUS")
                    time.sleep(0.6)
                    write_live(live, "")
                    escape_streak = 0
                else:
                    say(f"bright static menu after the skip (mean {mean:.1f}, motion {motion:.1f}): B to back out")
                    write_live(live, "B")
                    time.sleep(0.4)
                    write_live(live, "")
                    presses += 1
                time.sleep(a.every)
                continue
            escape_streak = 0

            # A still 3D scene is either the game waiting for movement or an in-engine cutscene
            # whose shot only advances on a button press: the Prologue's car/street shots sat at
            # motion 0.0 for 40+ samples until A was pressed repeatedly (prologue-cutscene1: the
            # first A moved it to a black transition and on to the next shot, while a single press
            # per 12 s from the old rule left it frozen). Alternate a walk step with a short A
            # burst, so a waiting gameplay scene also moves, and retrigger quickly (same_kind = 2).
            #
            # It retriggers itself (same_kind = 2), so it must give up on its own: a still *menu*
            # (the bar hub's save-point wall, mean 73 motion 0.1) matches it too, and mac-battle-probe
            # (2026-10-27) sat on that wall for 18.5 min / 220 rounds of walk+A because this branch
            # `continue`s before the stalled-shot escape below can count those samples. After
            # STUCK_LIMIT rounds it stops forcing same_kind and lets the fall-through presses (which
            # include B/PLUS) take over.
            if kind == "scene" and same_kind >= 3 and motion < 1.5 and 30 < mean < 160:
                if static_rounds < STUCK_LIMIT:
                    static_rounds += 1
                    say(f"static scene (mean {mean:.1f}, motion {motion:.1f}, round {static_rounds}): walk step + A burst")
                    if presses % 2 == 0:
                        write_live(live, "UP")
                        time.sleep(1.5)
                        write_live(live, "")
                        time.sleep(0.3)
                    for _ in range(2):
                        write_live(live, "A")
                        presses += 1
                        time.sleep(0.35)
                        write_live(live, "")
                        time.sleep(0.25)
                    same_kind = 2
                    time.sleep(a.every)
                    continue
                if static_rounds == STUCK_LIMIT:
                    static_rounds += 1
                    say(f"static scene after {STUCK_LIMIT} rounds (mean {mean:.1f}, motion {motion:.1f}): "
                        f"walk+A does not move it, trying B then PLUS")
                    write_live(live, "B")
                    time.sleep(0.5)
                    write_live(live, "")
                    time.sleep(0.4)
                    write_live(live, "PLUS")
                    time.sleep(0.5)
                    write_live(live, "")
                    presses += 2
                    time.sleep(a.every)
                    continue
                # A new kind resets static_rounds below; until then fall through to the generic cycle.

            # The bar hub (Records of Time): walk to the wall map and interact. The frame is the bar
            # interior with real animation (motion 4-30, mean 70-120) and no HUD.
            if kind == "scene" and 70 < mean < 120 and 4 < motion < 30:
                say(f"bar hub (mean {mean:.1f}, motion {motion:.1f}): patrol RIGHT/UP, A")
                for btn in ("RIGHT", "UP", "A", "RIGHT", "A"):
                    write_live(live, btn)
                    time.sleep(1.2 if btn in ("RIGHT", "UP") else 0.5)
                    write_live(live, "")
                    time.sleep(0.2)
                presses += 5
                time.sleep(a.every)
                continue

            # A dark menu/confirm page (mean 15-35, flat) is the pause menu or a confirm page: A
            # takes the highlighted default. (The cycle's DOWN+PLUS used to land on Photo Mode.)
            if kind == "menu" and 14 < mean < 36 and spread < 22 and time.time() - started > 120:
                # A YES/No page: A takes the highlighted default (No). The completed-save hub's
                # chapter-start confirm is the opposite ("YES / No", and A alone re-opens the page:
                # m3-save3), so taking Yes is OPT-IN (--take-yes) and only for that experiment.
                # Forcing Yes unconditionally is what replayed the intro in movie-deck-verify3: a
                # "return to title"-class page appeared after the movie chain, LEFT+A confirmed it,
                # and the intro movies started again (a second ev0000/ev0005 in that log).
                if a.take_yes:
                    say(f"YES/No page (mean {mean:.1f}, spread {spread:.1f}): LEFT, A (take Yes)")
                    write_live(live, "LEFT")
                    time.sleep(0.4)
                    write_live(live, "")
                    time.sleep(0.2)
                    presses += 1
                else:
                    say(f"YES/No page (mean {mean:.1f}, spread {spread:.1f}): A (keep the highlighted No)")
                write_live(live, "A")
                presses += 1
                time.sleep(a.settle)
                write_live(live, "")
                same_kind = 0
                time.sleep(a.every)
                continue

            # For HUD layouts not covered by the saved-image detector, retain the older motion
            # fallback. This is a driving hint; benchmark evidence still needs image/workload checks.
            if kind in ("scene", "dark-scene") and 20 < mean < 170 and black_share > 0.05 and motion > 6:
                battle_votes += 1
            else:
                battle_votes = 0
            if battle_votes >= 3:
                say(f"BATTLE: attack/dodge pattern for {FIGHT_BURST_SECONDS:g} s, then look again")
                fight(live, FIGHT_BURST_SECONDS)
                battle_votes = 0
                time.sleep(a.every)
                continue

            # The in-movie pause menu over a black movie is a *dark*, still frame (mean ~18 with the
            # black background dominating). A real black cutscene still moves; a fade has no spread.
            # Detected before the kind switch so that both "black" and "dark-scene" classifications
            # reach it: the way on is DOWN (Skip is the second item) then A. Pressing PLUS again here
            # would resume the movie (black -> PLUS -> resume -> black, which is what used to happen).
            #
            # Guard (2026-10-23, after the title screen stopped being a white flash): the boot/loading
            # screens and the now-dark title are also dark, still frames with black>0.45, and the rule
            # pressed DOWN+A on them, which opened a menu before the story had started. This state only
            # exists once playback has begun or after this driver itself opened the pause menu.
            elapsed = time.time() - started
            dark_menu = (pause_pending or elapsed > 150) and motion < 2.0 and 8 < mean < 60 and black_share > 0.45
            # A long movie's static shot (ev0025: a grey building, mean ~60, no motion, 6.5 fps in
            # that state) is neither a battle nor a prompt; the game will keep showing it. Skip it
            # from the pause menu: PLUS opens it, DOWN selects Skip, A confirms.
            if same_kind >= 20 and 40 < mean < 80 and motion < 1.0 and black_share > 0.15:
                say(f"static shot (mean {mean:.1f}, motion {motion:.1f}, {same_kind} samples): PLUS, DOWN, A to skip")
                write_live(live, "PLUS")
                time.sleep(1.0)
                write_live(live, "")
                time.sleep(0.5)
                write_live(live, "DOWN")
                time.sleep(0.6)
                write_live(live, "")
                time.sleep(0.3)
                write_live(live, "A")
                presses += 3
                time.sleep(a.settle)
                write_live(live, "")
                same_kind = 0
                time.sleep(max(0.0, a.every - a.settle))
                continue
            if dark_menu:
                say(f"dark still screen over black (mean {mean:.1f}, motion {motion:.1f}): DOWN to Skip, A to confirm")
                write_live(live, "DOWN")
                time.sleep(0.6)
                write_live(live, "")
                time.sleep(0.3)
                write_live(live, "A")
                presses += 2
                time.sleep(a.settle)
                write_live(live, "")
                pause_pending = False
                black_streak = 0
                time.sleep(max(0.0, a.every - a.settle))
                continue

            if kind == "black":
                black_streak += 1
                # The old rule pressed PLUS after 3 black samples (~4.5 s) — inside every movie
                # black-out, so it toggled the pause menu for the whole of ev0021. A cut to black
                # in a cutscene lasts seconds; only --black-patience samples of continuous black
                # (15 ≈ 15-22 s) with no picture in between counts as a stall.
                if same_kind < a.black_patience:
                    write_live(live, "")
                elif not pause_pending:
                    say(f"stalled black for {same_kind} samples: pressing PLUS")
                    write_live(live, "PLUS")
                    presses += 1
                    time.sleep(a.settle)
                    write_live(live, "")
                    pause_pending = True
                else:
                    # PLUS did not open a menu: this black stretch is the movie itself. Release and
                    # wait for it to move instead of toggling the menu on and off.
                    write_live(live, "")
                time.sleep(max(0.0, a.every - a.settle))
                continue

            black_before_menu = black_streak
            black_streak = 0
            pause_pending = False
            # The pause menu over a cutscene: bright, static, with the ~0.6 bright share the
            # loading screen never sits still at (its emblem rotates) and the title card exceeds.
            if kind == "bright" and 0.5 < bright < 0.75 and motion < 3 and black_before_menu >= 3:
                say("pause menu over the cutscene: DOWN to Skip, A to confirm")
                write_live(live, "DOWN")
                time.sleep(0.6)
                write_live(live, "")
                time.sleep(0.3)
                write_live(live, "A")
                presses += 2
                time.sleep(a.settle)
                write_live(live, "")
                time.sleep(max(0.0, a.every - a.settle))
                continue

            # Every screen the game uses to gate progress takes A. A screen that ignores it for
            # several polls gets a different input: a white flash that never ends is usually a
            # prompt waiting for something else (a confirmation, a skip), not an A prompt.
            #
            # The cycle starts at A (index 0) and only after the 4 A presses above; `same_kind // 2`
            # used to index it, which skipped A and B entirely -- a screen that only closes on B (the
            # Naive Angel / options cards are Close-on-B) then never got one, and 600 s of driving sat
            # on "Cute Masking 1/2" while it cycled UP/RIGHT/LEFT/PLUS. Subtract the 4 first and the
            # whole cycle is reachable.
            if same_kind >= 4:
                cycle = ["A", "B", "A", "RIGHT", "A", "LEFT", "A", "B"]
                button = cycle[(same_kind - 4) % len(cycle)]
            else:
                button = "A"
            write_live(live, button)
            presses += 1
            time.sleep(a.settle)
            write_live(live, "")
            time.sleep(max(0.0, a.every - a.settle))
    except KeyboardInterrupt:
        say("interrupted")
    finally:
        write_live(live, "")
        say(f"autodrive: {presses} press(es) over {time.time() - started:.0f} s")
        if log:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
