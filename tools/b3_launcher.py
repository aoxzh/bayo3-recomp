#!/usr/bin/env python3
"""The Bayonetta 3 launcher: one window for the settings, the controller check and Play.

    scripts/b3_launcher.sh          # Linux/Deck, or: python3 tools/b3_launcher.py
    python tools/b3_launcher.py     # Windows (no shell wrapper needed; see --check)

It edits the same private/b3.conf that the entry scripts read (config/b3.conf.example documents every key) and it
starts the game through the host's entry script -- scripts/b3_play.sh on the Deck and the desktop, and
scripts/b3_win_play.sh on Windows -- so a setting means the same thing whether it is set here, in the file or in the
environment. Everything that is not UI lives in tools/b3_launcher_config.py, which tests/test_b3_launcher_config.py
covers without a display; this file only holds widgets, the log tail and the gamepad watcher.

The window has two layers: a home screen (the art in tools/launcher_assets, the name, and a menu -- Start game,
Settings, Game log, Quit) and a settings panel over a blurred copy of the art. Pillow draws the art when it is
installed; without it the same layout sits on a flat background.

The controller works with no other input device: D-pad or left stick moves through the menu or the settings, A
accepts (flips a switch, steps a list), B goes back (settings -> tabs -> home), Start starts the game. The pad is read through
SDL3 with ctypes where SDL3 is installed -- the same library the runtime uses, so no python package is needed -- and
through XInput on Windows, which is the API the game itself reads there.
"""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
import queue
import re
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_launcher_config as cfg  # noqa: E402

# ---- the look ---------------------------------------------------------------------------------------------------
# Taken from the background art (tools/launcher_assets/background.jpg): a rain-dark night city, cyan neon for focus
# and selection, gold for the one thing to do next (Start game, the switch that is on), magenta used sparingly.
BG = '#0b0a14'          # behind everything, and the colour the art is darkened towards
PANEL = '#13111f'       # the frosted settings panel (the art under it is blurred and tinted to this)
FIELD = '#1d1a2e'       # a value box on the panel
HOVER = '#221e36'
LINE = '#2e2944'        # hairlines and idle borders
TEXT = '#f1ecf7'
MUTED = '#a59fbd'
FAINT = '#6d6787'
CYAN = '#5fe3ef'
CYAN_DIM = '#2b6f78'
GOLD = '#e8b75c'
GOLD_DIM = '#6e5631'
MAGENTA = '#d651dc'
OFF = '#383352'         # a switch that is off
RED = '#ff5f7a'         # an error in the status line
BACKGROUND_ART = Path(__file__).resolve().parent / 'launcher_assets' / 'background.jpg'

# Fonts are resolved once the first Tk root exists (the family list needs one): the first family this Tk has wins.
# Asking Tk for a missing family silently gives a bitmap fallback, so nothing here is taken on trust.
_UI_CANDIDATES = ('Segoe UI Variable Text', 'Segoe UI', 'Cantarell', 'Noto Sans', 'DejaVu Sans', 'Helvetica Neue',
                  'Helvetica', 'Arial')
_DISPLAY_CANDIDATES = ('Georgia', 'Cambria', 'Noto Serif', 'DejaVu Serif', 'Liberation Serif', 'Times New Roman',
                       'Times')
_MONO_CANDIDATES = ('Cascadia Mono', 'Consolas', 'DejaVu Sans Mono', 'Noto Sans Mono', 'Menlo', 'Courier New')
UI = 'TkDefaultFont'
DISPLAY = 'TkDefaultFont'
MONO = 'TkFixedFont'
SCALE = 1.0             # physical pixels per 96-dpi pixel, for lengths drawn on canvases (points scale by themselves)


def _pick_fonts(root):
    global UI, DISPLAY, MONO, SCALE
    try:
        import tkinter.font as tkfont
        available = {name.lower() for name in tkfont.families(root)}
    except Exception:  # no font module, or no display: the platform defaults are fine
        available = set()

    def first(candidates, fallback):
        return next((name for name in candidates if name.lower() in available), fallback)

    UI = first(_UI_CANDIDATES, UI)
    DISPLAY = first(_DISPLAY_CANDIDATES, UI)
    MONO = first(_MONO_CANDIDATES, MONO)
    try:
        SCALE = max(1.0, root.winfo_fpixels('1i') / 96.0)
    except tk.TclError:
        SCALE = 1.0


def px(n):
    """A 96-dpi length in this display's pixels."""
    return int(round(n * SCALE))


def _dpi_aware():
    """Windows draws a DPI-unaware window as a stretched bitmap, which blurs every glyph; ask for real pixels first."""
    if os.name != 'nt':
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def _rgb(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5))


class Backdrop:
    """The art, sized to cover the window and darkened where text sits. Pillow does the work when it is installed;
    without it the launcher keeps the same layout on a flat background (Tk alone can neither scale nor blur)."""

    def __init__(self, path=BACKGROUND_ART):
        self.source = None
        self.cache = {}
        try:
            from PIL import Image  # noqa: F401  (optional dependency)
            if path.exists():
                self.source = Image.open(path).convert('RGB')
        except Exception:
            self.source = None

    def available(self):
        return self.source is not None

    def _cover(self, w, h):
        from PIL import Image
        sw, sh = self.source.size
        scale = max(w / sw, h / sh)
        img = self.source.resize((max(w, int(sw * scale + 0.5)), max(h, int(sh * scale + 0.5))), Image.LANCZOS)
        left, top = (img.width - w) // 2, int((img.height - h) * 0.35)  # keep the moon and spires in view
        return img.crop((left, top, left + w, top + h))

    @staticmethod
    def _ramp(length, stops, horizontal):
        """A 1-pixel strip of darkness values interpolated between (position 0..1, alpha 0..1) stops."""
        from PIL import Image
        values = []
        for i in range(length):
            t = i / max(1, length - 1)
            for (p0, a0), (p1, a1) in zip(stops, stops[1:]):
                if p0 <= t <= p1:
                    a = a0 + (a1 - a0) * (t - p0) / max(1e-6, p1 - p0)
                    break
            else:
                a = stops[-1][1]
            values.append(int(255 * a))
        strip = Image.new('L', (length, 1) if horizontal else (1, length))
        strip.putdata(values)
        return strip

    def home(self, w, h):
        key = ('home', w, h)
        if key not in self.cache:
            from PIL import Image, ImageChops, ImageTk
            img = self._cover(w, h)
            across = self._ramp(w, [(0, 0.86), (0.42, 0.45), (0.7, 0.12), (1, 0.18)], True).resize((w, h))
            down = self._ramp(h, [(0, 0.35), (0.12, 0.05), (0.55, 0.05), (1, 0.85)], False).resize((w, h))
            mask = ImageChops.lighter(across, down)
            img = Image.composite(Image.new('RGB', (w, h), _rgb(BG)), img, mask)
            self.cache = {k: v for k, v in self.cache.items() if k[0] != 'home'}
            self.cache[key] = ImageTk.PhotoImage(img)
        return self.cache[key]

    def settings(self, w, h, panel, radius):
        key = ('settings', w, h, panel)
        if key not in self.cache:
            from PIL import Image, ImageDraw, ImageFilter, ImageTk
            img = self._cover(w, h).filter(ImageFilter.GaussianBlur(px(14)))
            img = Image.blend(img, Image.new('RGB', (w, h), _rgb(BG)), 0.55)
            mask = Image.new('L', (w, h), 0)
            ImageDraw.Draw(mask).rounded_rectangle(panel, radius=radius, fill=int(255 * 0.9))
            img = Image.composite(Image.new('RGB', (w, h), _rgb(PANEL)), img, mask)
            self.cache = {k: v for k, v in self.cache.items() if k[0] != 'settings'}
            self.cache[key] = ImageTk.PhotoImage(img)
        return self.cache[key]


# SDL3's shared library, which the launcher reads the controller through (ctypes, no python bindings). The name
# differs per host, and Windows keeps the DLL wherever the SDL3 build put it: SDL3.dll beside the game, or in any
# directory on PATH.
SDL_LIBRARY = os.environ.get('B3_SDL3') or ('SDL3.dll' if os.name == 'nt' else 'libSDL3.so.0')

# The hosts this launcher runs on, named the way the player sees them in the title bar.
HOST_LABEL = 'Windows, Steam Deck and desktop' if os.name == 'nt' else 'Steam Deck and desktop'


# ---- the controller ---------------------------------------------------------------------------------------------

class XiPad:
    """XInput through ctypes: the Windows backend, used when SDL3 is not installed.

    The game itself reads its controller through XInput on this host (runtime/src/platform_win.cpp), so a launcher
    that needed SDL would show "SDL unavailable" on a machine where the game's own pad works fine. XInput exposes no
    device name and no analog triggers here, which is all this launcher needs to move a focus ring.
    """

    BUTTONS = {
        'a': 0x1000, 'b': 0x2000, 'x': 0x4000, 'y': 0x8000,
        'back': 0x0020, 'start': 0x0010,
        'up': 0x0001, 'down': 0x0002, 'left': 0x0004, 'right': 0x0008,
    }

    def __init__(self):
        self.lib = None
        self.slot = 0
        self.names = []
        self.error = ''
        self.last = {}
        if os.environ.get('B3_LAUNCHER_NO_PAD'):
            self.error = 'pad disabled'
            return
        lib = None
        for name in ('XInput1_4.dll', 'XInput1_3.dll', 'xinput1_4.dll', 'xinput1_3.dll'):
            try:
                lib = ctypes.WinDLL(name)
                break
            except OSError:
                continue
        if lib is None:
            self.error = 'XInput unavailable'
            return
        lib.XInputGetState.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
        lib.XInputGetState.restype = ctypes.c_uint32
        self.lib = lib
        self.refresh()

    def refresh(self):
        if not self.lib:
            return
        self.names = []
        self.slot = 0
        for slot in range(4):
            if self._state(slot) is not None:
                self.slot = slot
                self.names = [f'XInput controller {slot}']
                break

    def _state(self, slot):
        buf = ctypes.create_string_buffer(16)  # XINPUT_STATE is 16 bytes: dwPacketNumber + XINPUT_GAMEPAD
        if self.lib.XInputGetState(slot, buf) != 0:  # ERROR_SUCCESS
            return None
        # XINPUT_GAMEPAD starts at offset 4: wButtons (u16), then four u8 triggers/axes.
        return int.from_bytes(buf.raw[4:6], 'little')

    def pressed(self):
        if not self.lib:
            return set()
        buttons = self._state(self.slot)
        if buttons is None:
            self.refresh()
            buttons = self._state(self.slot) if self.names else None
        if buttons is None:
            self.last.clear()
            return set()
        down = {name for name, bit in self.BUTTONS.items() if buttons & bit}
        fresh = {name for name in down if not self.last.get(name)}
        self.last = {name: name in down for name in self.BUTTONS}
        return fresh

    def close(self):
        self.lib = None
        self.names = []
        self.last.clear()


class Pad:
    """SDL3 through ctypes: the pads SDL can see, and the buttons pressed since the last poll.

    Falls back to XInput on Windows, where SDL3 is not necessarily installed but the game's own pad path works.
    """

    BUTTONS = {'a': 0, 'b': 1, 'x': 2, 'y': 3, 'back': 4, 'start': 6, 'up': 11, 'down': 12, 'left': 13, 'right': 14}

    def __init__(self):
        self.lib = None
        self.ids = []
        self.pads = []
        self.names = []
        self.last = {}
        self.error = ''
        self.fallback = None
        if os.environ.get('B3_LAUNCHER_NO_PAD'):  # for screenshots and scripted runs: never touch a real pad
            self.error = 'pad disabled'
            return
        try:
            lib = ctypes.CDLL(SDL_LIBRARY)
            lib.SDL_InitSubSystem.argtypes = [ctypes.c_uint32]
            lib.SDL_InitSubSystem.restype = ctypes.c_bool
            lib.SDL_QuitSubSystem.argtypes = [ctypes.c_uint32]
            lib.SDL_QuitSubSystem.restype = None
            lib.SDL_GetError.argtypes = []
            lib.SDL_GetError.restype = ctypes.c_char_p
            lib.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            lib.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)
            lib.SDL_free.argtypes = [ctypes.c_void_p]
            lib.SDL_free.restype = None
            lib.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
            lib.SDL_OpenGamepad.restype = ctypes.c_void_p
            lib.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            lib.SDL_CloseGamepad.restype = None
            lib.SDL_GamepadConnected.argtypes = [ctypes.c_void_p]
            lib.SDL_GamepadConnected.restype = ctypes.c_bool
            lib.SDL_GetGamepadName.argtypes = [ctypes.c_void_p]
            lib.SDL_GetGamepadName.restype = ctypes.c_char_p
            lib.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.SDL_GetGamepadButton.restype = ctypes.c_bool
            lib.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            lib.SDL_GetGamepadAxis.restype = ctypes.c_int16
            lib.SDL_PumpEvents.argtypes = []
            lib.SDL_PumpEvents.restype = None
        except (OSError, AttributeError) as exc:
            # Windows: SDL3 is a separate install and is not needed by the game (it reads XInput directly), so fall
            # back to XInput instead of showing "SDL unavailable" on a machine where the pad does work.
            self._fallback_to_xinput(f'SDL unavailable: {exc}')
            return
        if not lib.SDL_InitSubSystem(0x2000):  # SDL_INIT_GAMEPAD
            self._fallback_to_xinput(f'SDL gamepad initialization failed: {self._sdl_error(lib)}')
            return
        self.lib = lib
        self.refresh()

    def _fallback_to_xinput(self, reason):
        if os.name != 'nt':
            self.error = reason
            return
        self.fallback = XiPad()
        if self.fallback.lib is not None:
            # XInput loaded, so this machine's pad path works exactly as the game's does; whether one is plugged in
            # is a normal state, not an error. Reporting SDL's message here would claim the controller is unusable.
            self.error = ''
            self.names = list(self.fallback.names)
        else:
            self.error = self.fallback.error or reason

    @staticmethod
    def _sdl_error(lib):
        raw = lib.SDL_GetError()
        return raw.decode('utf-8', 'replace') if raw else 'unknown SDL error'

    def _drop_disconnected(self):
        # An enumeration failure must not keep a disconnected primary handle alive.
        old_primary = self.ids[0] if self.ids else None
        connected = []
        for instance_id, pad, name in zip(self.ids, self.pads, self.names):
            if self.lib.SDL_GamepadConnected(pad):
                connected.append((instance_id, pad, name))
            else:
                self.lib.SDL_CloseGamepad(pad)
        self.ids = [item[0] for item in connected]
        self.pads = [item[1] for item in connected]
        self.names = [item[2] for item in connected]
        if not self.ids or self.ids[0] != old_primary:
            self.last.clear()

    def refresh(self):
        """Follow SDL's connected-device order without reopening unchanged instances."""
        if self.fallback is not None:
            self.fallback.refresh()
            self.names = list(self.fallback.names)
            return
        if not self.lib:
            return
        lib = self.lib
        lib.SDL_PumpEvents()
        count = ctypes.c_int(0)
        found = lib.SDL_GetGamepads(ctypes.byref(count))
        if not found:
            self.error = f'SDL gamepad scan failed: {self._sdl_error(lib)}'
            self._drop_disconnected()
            return
        try:
            # SDL owns this allocation even when the list is empty. Keep each instance once.
            instances = list(dict.fromkeys(found[i] for i in range(max(0, count.value))))
        finally:
            lib.SDL_free(found)

        old = {instance_id: pad for instance_id, pad in zip(self.ids, self.pads)}
        old_primary = self.ids[0] if self.ids else None
        retained, opened = set(), []
        ids, pads, names = [], [], []
        error = ''
        try:
            for instance_id in instances:
                pad = old.get(instance_id)
                reused = bool(pad and lib.SDL_GamepadConnected(pad))
                if not reused:
                    pad = lib.SDL_OpenGamepad(instance_id)
                    if not pad:
                        if not error:
                            error = f'Could not open controller {instance_id}: {self._sdl_error(lib)}'
                        continue
                    opened.append(pad)
                    if not lib.SDL_GamepadConnected(pad):  # it disconnected between enumeration and open
                        lib.SDL_CloseGamepad(pad)
                        opened.pop()
                        continue
                else:
                    retained.add(instance_id)
                raw = lib.SDL_GetGamepadName(pad)
                ids.append(instance_id)
                pads.append(pad)
                names.append(raw.decode('utf-8', 'replace') if raw else f'controller {instance_id}')
        except Exception:
            for pad in opened:
                lib.SDL_CloseGamepad(pad)
            raise
        for instance_id, pad in old.items():
            if instance_id not in retained:
                lib.SDL_CloseGamepad(pad)
        self.ids, self.pads, self.names, self.error = ids, pads, names, error
        # Non-primary changes must not make an already-held button fire again.
        if not ids or ids[0] != old_primary or ids[0] not in retained:
            self.last.clear()

    def pressed(self):
        """The buttons that went down since the previous call, so a held button does not repeat."""
        if self.fallback is not None:
            fresh = self.fallback.pressed()
            self.names = list(self.fallback.names)
            return fresh
        if not self.lib:
            return set()
        self.lib.SDL_PumpEvents()  # hotplug must advance even when there are no open pads
        if self.pads and not self.lib.SDL_GamepadConnected(self.pads[0]):
            self.refresh()
        if not self.pads:
            return set()
        pad = self.pads[0]
        down = {name for name, code in self.BUTTONS.items() if self.lib.SDL_GetGamepadButton(pad, code)}
        # The stick doubles as the D-pad (on a Deck it is the natural way to move a focus ring).
        for name, axis, sign in (('left', 0, -1), ('right', 0, 1), ('up', 1, -1), ('down', 1, 1)):
            if self.lib.SDL_GetGamepadAxis(pad, axis) * sign > 20000:
                down.add(name)
        fresh = {name for name in down if not self.last.get(name)}
        self.last = {name: name in down for name in self.BUTTONS}
        return fresh

    def close(self):
        """Release each owned gamepad and the successfully initialized subsystem once."""
        if self.fallback is not None:
            self.fallback.close()
            self.fallback = None
        lib, self.lib = self.lib, None
        if lib:
            for pad in self.pads:
                lib.SDL_CloseGamepad(pad)
            lib.SDL_QuitSubSystem(0x2000)
        self.ids.clear()
        self.pads.clear()
        self.names.clear()
        self.last.clear()


# ---- the window -------------------------------------------------------------------------------------------------

# ---- widgets ----------------------------------------------------------------------------------------------------
# Drawn here rather than taken from ttk: a theme's checkbox is a few pixels wide and its combobox is a grey slab on
# Windows. Each takes the keyboard focus, shows it with a cyan ring, and is driven by the same tk variable the
# settings model reads, so the pad navigation only has to move the focus and nudge values.

def _round_rect(canvas, x1, y1, x2, y2, r, **kw):
    points = (x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
              x1, y2, x1, y2 - r, x1, y1 + r, x1, y1)
    return canvas.create_polygon(points, smooth=True, **kw)


class Toggle(tk.Frame):
    """A switch with its state in words beside it (a pad user has no cursor to read a tick against)."""

    def __init__(self, master, variable):
        super().__init__(master, bg=PANEL, takefocus=1, highlightthickness=px(2), highlightbackground=PANEL,
                         highlightcolor=CYAN)
        self.var = variable
        self.word = tk.Label(self, bg=PANEL, fg=MUTED, font=(UI, 10), width=3, anchor='e')
        self.word.pack(side='left', padx=(px(4), px(8)))
        self.w, self.h = px(46), px(26)
        self.track = tk.Canvas(self, width=self.w, height=self.h, bg=PANEL, highlightthickness=0, cursor='hand2')
        self.track.pack(side='left', padx=(0, px(2)), pady=px(2))
        for target in (self, self.word, self.track):
            target.bind('<Button-1>', self._click)
        self.bind('<space>', lambda _e: self.flip())
        self.bind('<Return>', lambda _e: self.flip())
        self.var.trace_add('write', lambda *_a: self._draw())
        self._draw()

    def _click(self, _event):
        self.focus_set()
        self.flip()

    def flip(self):
        self.var.set(not self.var.get())

    def _draw(self):
        on = bool(self.var.get())
        c, w, h = self.track, self.w, self.h
        c.delete('all')
        _round_rect(c, 1, 1, w - 1, h - 1, h // 2, fill=GOLD if on else OFF, outline='')
        d = h - px(8)
        x = w - px(4) - d if on else px(4)
        c.create_oval(x, px(4), x + d, px(4) + d, fill=PANEL if on else MUTED, outline='')
        self.word.configure(text='On' if on else 'Off', fg=GOLD if on else MUTED)


class Choice(tk.Frame):
    """‹ value ›: the arrows (or Left/Right) step through the list, a click on the value opens all of it."""

    def __init__(self, master, variable, labels, width=30):
        super().__init__(master, bg=FIELD, takefocus=1, highlightthickness=px(2), highlightbackground=LINE,
                         highlightcolor=CYAN)
        self.var, self.labels = variable, list(labels)
        self.on_change = None
        arrow = dict(bg=FIELD, fg=MUTED, font=(UI, 13), cursor='hand2', padx=px(10))
        self.left = tk.Label(self, text='‹', **arrow)
        self.right = tk.Label(self, text='›', **arrow)
        self.value = tk.Label(self, textvariable=variable, bg=FIELD, fg=TEXT, font=(UI, 10), width=width,
                              anchor='center', cursor='hand2', pady=px(4))
        self.left.pack(side='left')
        self.value.pack(side='left', fill='x', expand=True)
        self.right.pack(side='left')
        self.left.bind('<Button-1>', lambda _e: (self.focus_set(), self.step(-1)))
        self.right.bind('<Button-1>', lambda _e: (self.focus_set(), self.step(1)))
        self.value.bind('<Button-1>', lambda _e: (self.focus_set(), self.open_menu()))
        for a in (self.left, self.right):
            a.bind('<Enter>', lambda _e, a=a: a.configure(fg=CYAN))
            a.bind('<Leave>', lambda _e, a=a: a.configure(fg=MUTED))
        self.bind('<Left>', lambda _e: self.step(-1))
        self.bind('<Right>', lambda _e: self.step(1))
        self.bind('<Return>', lambda _e: self.open_menu())
        self.bind('<space>', lambda _e: self.open_menu())

    def step(self, direction):
        current = self.var.get()
        index = self.labels.index(current) if current in self.labels else 0
        self.var.set(self.labels[(index + direction) % len(self.labels)])
        if self.on_change:
            self.on_change()

    def open_menu(self):
        menu = tk.Menu(self, tearoff=0, bg=PANEL, fg=TEXT, activebackground=FIELD, activeforeground=CYAN,
                       selectcolor=GOLD, font=(UI, 10), borderwidth=0, relief='flat')
        for label in self.labels:
            menu.add_radiobutton(label=label, variable=self.var, value=label,
                                 command=lambda: self.on_change and self.on_change())
        try:
            menu.tk_popup(self.winfo_rootx(), self.winfo_rooty() + self.winfo_height())
        finally:
            menu.grab_release()


class Field(tk.Frame):
    """A text or number box with a quiet border that turns cyan while typing."""

    def __init__(self, master, variable, width=22, placeholder=''):
        super().__init__(master, bg=FIELD, highlightthickness=px(2), highlightbackground=LINE)
        self.entry = tk.Entry(self, textvariable=variable, width=width, bg=FIELD, fg=TEXT, insertbackground=CYAN,
                              relief='flat', borderwidth=0, font=(UI, 10), highlightthickness=0,
                              selectbackground=CYAN_DIM, selectforeground=TEXT)
        self.entry.pack(fill='x', padx=px(10), pady=px(5))
        # An empty box means "the runtime's default": say which, in faint text over the box, until something is typed.
        self.ghost = tk.Label(self, text=placeholder, bg=FIELD, fg=FAINT, font=(UI, 10), anchor='w')
        self.ghost.bind('<Button-1>', lambda _e: self.entry.focus_set())
        self.var = variable
        variable.trace_add('write', lambda *_a: self._ghost())
        self.entry.bind('<FocusIn>', lambda _e: (self.configure(highlightbackground=CYAN), self._ghost()))
        self.entry.bind('<FocusOut>', lambda _e: (self.configure(highlightbackground=LINE), self._ghost()))
        self.after_idle(self._ghost)

    def _ghost(self):
        if self.ghost['text'] and not self.var.get() and self.focus_get() is not self.entry:
            self.ghost.place(x=px(10), rely=0.5, anchor='w')
        else:
            self.ghost.place_forget()

    def focus_set(self):
        self.entry.focus_set()


class Button(tk.Label):
    """A flat button on the panel: 'outline' is a gold-edged action, 'ghost' just words."""

    LOOKS = {
        'outline': dict(fg=GOLD, edge=GOLD_DIM),
        'ghost': dict(fg=MUTED, edge=PANEL),
    }

    def __init__(self, master, text, command, look='ghost'):
        style = self.LOOKS[look]
        super().__init__(master, text=text, bg=PANEL, fg=style['fg'], cursor='hand2', takefocus=1,
                         font=(UI, 10), padx=px(16), pady=px(6), highlightthickness=px(1),
                         highlightbackground=style['edge'], highlightcolor=CYAN)
        self.command = command
        self.bind('<Button-1>', lambda _e: self.command())
        self.bind('<Return>', lambda _e: self.command())
        self.bind('<space>', lambda _e: self.command())
        self.bind('<Enter>', lambda _e: self.configure(bg=HOVER))
        self.bind('<Leave>', lambda _e: self.configure(bg=PANEL))


# ---- the window -------------------------------------------------------------------------------------------------
# Two layers, the way the recomp launchers players already know are built (Zelda64Recomp's main menu, the big game
# launchers): a home screen that is the art, the name and a short menu read from the lower left, every entry
# reachable with a pad; and a settings panel that slides over a blurred copy of the same art. Everything is drawn on
# one canvas so the text sits on the art without boxes; only the controls are real widgets, placed with
# create_window and shown for the group on screen.

class Launcher(tk.Tk):
    MENU = ('play', 'settings', 'log', 'quit')

    def __init__(self):
        super().__init__()
        self.root = cfg.repo_root()
        self.paths = cfg.paths(self.root)
        if self.paths['conf'].exists():
            self.conf = cfg.Conf.load(self.paths['conf'])
        else:  # start from the documented template, so the first save keeps its comments
            self.conf = cfg.Conf.load(self.paths['template'])
        self.vars = {}
        self.rows = {}
        self.windows = {}       # setting key -> the canvas window item holding its control
        self.focusable = []
        self.group = cfg.GROUPS[0]
        self.view = 'home'
        self.menu_index = 0
        self.nav_mode = False
        self.focus_index = 0
        self.proc = None
        self.stopping = False
        self.status_text, self.status_bad = '', False
        self.help_text = ''
        self.pad = Pad()
        self.pad_tick = 0
        self.log_shown = False
        self._layout_job = None
        # A setup or update run through tools/b3_win.py (Windows): its output lines, the step it is on, the progress.
        self.task = None
        self.task_kind = ''
        self.task_lines = queue.Queue()
        self.task_log = []
        self.task_step, self.task_done, self.task_total = '', 0, 1

        _pick_fonts(self)    # before any widget: every font and canvas length below reads the result
        self.backdrop = Backdrop()
        self.title('Bayonetta 3')
        # Sized to fit above a taskbar or a Deck's overlay: the screen less the title bar and the bar, centred.
        screen_w, screen_h = self.winfo_screenwidth(), self.winfo_screenheight()
        win_w, win_h = min(px(1120), screen_w - px(40)), min(px(760), screen_h - px(140))
        self.geometry(f'{win_w}x{win_h}+{max(0, (screen_w - win_w) // 2)}+{max(0, (screen_h - win_h - px(90)) // 2)}')
        self.minsize(min(px(940), win_w), min(px(620), win_h))
        self.configure(bg=BG)
        self.canvas = tk.Canvas(self, bg=BG, highlightthickness=0, takefocus=0)
        self.canvas.pack(fill='both', expand=True)
        self._controls()
        self.log_view = tk.Text(self.canvas, bg='#08070f', fg='#d8d2e6', insertbackground=TEXT, font=(MONO, 9),
                                borderwidth=0, highlightthickness=1, highlightbackground=LINE, padx=px(12),
                                pady=px(8), wrap='char')
        self.log_view.configure(state='disabled')
        self.log_item = self.canvas.create_window(0, 0, window=self.log_view, anchor='nw', state='hidden')
        self._load_values()
        self._set_status(self._ready_text())
        self.canvas.bind('<Configure>', lambda _e: self._schedule_layout())
        self.bind('<Escape>', lambda _e: self._escape())
        self.bind('<Control-Return>', lambda _e: self.play())
        self.bind('<Control-s>', lambda _e: self.view == 'settings' and self.save())
        for key, step in (('<Up>', -1), ('<Down>', 1)):
            self.bind(key, lambda e, s=step: self._arrow(e, s))
        self.bind('<Return>', lambda e: self._home_enter(e))
        self.after(120, self._poll_pad)
        self.after(300, self._poll_proc)
        self.after(300, self._poll_task)
        self.after(1000, self._poll_log)

    def destroy(self):
        self.pad.close()
        super().destroy()

    # -- controls (created once, placed by the layout) ---------------------------------------------------------------
    def _controls(self):
        for setting in cfg.SETTINGS:
            widget = self._widget(setting)
            self.rows[setting.key] = (None, widget)
            self.windows[setting.key] = self.canvas.create_window(0, 0, window=widget, anchor='e', state='hidden')
            self.focusable.append(widget)
            target = widget.entry if isinstance(widget, Field) else widget
            target.bind('<FocusIn>', lambda _e, w=widget: self._focus_moved(w), add='+')
        self.save_button = Button(self.canvas, 'Save', self.save, look='outline')
        self.reset_button = Button(self.canvas, 'Reset to defaults', self.reset, look='ghost')
        self.save_item = self.canvas.create_window(0, 0, window=self.save_button, anchor='e', state='hidden')
        self.reset_item = self.canvas.create_window(0, 0, window=self.reset_button, anchor='e', state='hidden')

    def _widget(self, setting):
        if setting.kind == 'bool':
            var = tk.BooleanVar(value=False)
            self.vars[setting.key] = var
            var.trace_add('write', lambda *_a: self._refresh_status())
            return Toggle(self.canvas, var)
        var = tk.StringVar()
        self.vars[setting.key] = var
        if setting.kind == 'choice':
            labels = [label for _v, label in setting.choices]
            box = Choice(self.canvas, var, labels, width=min(46, max(14, max(len(t) for t in labels) + 3)))
            box.on_change = self._refresh_status
            return box
        var.trace_add('write', lambda *_a: self._refresh_status())
        return Field(self.canvas, var, width=12 if setting.kind == 'int' else 30,
                     placeholder=setting.hint or ('Off' if setting.key == 'discord_app_id' else 'Default'))

    # -- layout -------------------------------------------------------------------------------------------------------
    def _schedule_layout(self):
        if self._layout_job is not None:
            self.after_cancel(self._layout_job)
        self._layout_job = self.after(60, self.layout)

    def layout(self):
        self._layout_job = None
        c = self.canvas
        w, h = max(c.winfo_width(), 2), max(c.winfo_height(), 2)
        c.delete('chrome')
        for item in self.windows.values():
            c.itemconfigure(item, state='hidden')
        for item in (self.save_item, self.reset_item, self.log_item):
            c.itemconfigure(item, state='hidden')
        if self.view == 'home':
            self._layout_home(w, h)
        else:
            self._layout_settings(w, h)
        c.tag_lower('backdrop')

    def _text(self, x, y, text, font, fill, anchor='w', tags=(), **kw):
        return self.canvas.create_text(x, y, text=text, font=font, fill=fill, anchor=anchor,
                                       tags=('chrome',) + tuple(tags), **kw)

    def _brand(self, x, y, size=1.0):
        c = self.canvas
        cx = x
        for ch in 'RECOMP':  # letter-spaced by hand: Tk has no tracking
            item = self._text(cx, y, ch, (UI, int(9 * size), 'bold'), CYAN)
            cx = c.bbox(item)[2] + px(5 * size)
        name = self._text(x - px(2), y + px(42 * size), 'Bayonetta', (DISPLAY, int(40 * size)), TEXT)
        self._text(c.bbox(name)[2] + px(12 * size), y + px(40 * size), '3', (DISPLAY, int(50 * size), 'italic'), GOLD)
        c.create_line(x, y + px(82 * size), x + px(48 * size), y + px(82 * size), fill=GOLD, width=px(2),
                      tags=('chrome',))

    def _layout_home(self, w, h):
        c = self.canvas
        if self.backdrop.available():
            c.create_image(0, 0, image=self.backdrop.home(w, h), anchor='nw', tags=('chrome', 'backdrop'))
        left = px(72)
        self._brand(left, px(70))
        self._text(left, px(178), 'Native build of the Switch game · bring your own game files', (UI, 10), MUTED)
        # The menu: large serif words, the selected one gold with a cyan diamond, as a pad-first menu reads.
        labels = {'play': 'Stop game' if self.proc is not None else 'Start game', 'settings': 'Settings',
                  'log': 'Hide game log' if self.log_shown else 'Game log', 'quit': 'Quit',
                  'setup': 'Setting up…' if self.task else 'Set up from my dump',
                  'rebuild': 'Rebuilding…' if self.task else 'Rebuild'}
        step = px(54)
        menu = self._menu()
        top = h - px(84) - step * (len(menu) - 1)
        for i, key in enumerate(menu):
            y = top + i * step
            selected = i == self.menu_index
            big = key in ('play', 'setup')
            font = (DISPLAY, 26 if big else 19, 'bold' if big and selected else 'normal')
            item = self._text(left, y, labels[key], font, GOLD if selected else (TEXT if big else MUTED),
                              tags=(f'menu{i}',))
            if selected:
                d = px(6)
                c.create_polygon(left - px(26), y, left - px(26) + d, y - d, left - px(26) + 2 * d, y,
                                 left - px(26) + d, y + d, fill=CYAN, outline='', tags=('chrome',))
                x2 = c.bbox(item)[2]
                c.create_line(left, y + px(22 if big else 17), x2, y + px(22 if big else 17), fill=GOLD_DIM,
                              width=px(1), tags=('chrome',))
            c.tag_bind(f'menu{i}', '<Enter>', lambda _e, i=i: self._menu_select(i))
            c.tag_bind(f'menu{i}', '<Button-1>', lambda _e, i=i: (self._menu_select(i), self._menu_activate()))
            c.tag_bind(f'menu{i}', '<Enter>', lambda _e: c.configure(cursor='hand2'), add='+')
            c.tag_bind(f'menu{i}', '<Leave>', lambda _e: c.configure(cursor=''))
        # The state of things, lower right: can it start, is a controller there, what will it run at.
        right, y = w - px(64), h - px(84)
        lines = [(self._run_state(), self.status_bad), (self._pad_text(), False), (self._describe(), False)]
        dots = [self._run_colour(), self._pad_colour(), None]
        for (text, bad), dot in zip(reversed(lines), reversed(dots)):
            item = self._text(right, y, text, (UI, 10), RED if bad else (TEXT if dot else MUTED), anchor='e')
            if dot:
                x1 = c.bbox(item)[0] - px(16)
                c.create_oval(x1, y - px(4), x1 + px(8), y + px(4), fill=dot, outline='', tags=('chrome',))
            y -= px(26)
        if self.task is not None:  # the setup or update's progress, above the state lines
            bar_w, bar_y = px(300), y - px(10)
            c.create_rectangle(right - bar_w, bar_y, right, bar_y + px(6), fill=LINE, outline='', tags=('chrome',))
            done = right - bar_w + int(bar_w * min(1.0, self.task_done / max(1, self.task_total)))
            c.create_rectangle(right - bar_w, bar_y, done, bar_y + px(6), fill=CYAN, outline='', tags=('chrome',))
        self._text(right, h - px(40), '↑ ↓ choose   ·   Enter / A confirm   ·   Esc quit', (UI, 8), FAINT, anchor='e')
        if self.log_shown:
            x1, y1 = int(w * 0.46), px(150)
            c.coords(self.log_item, x1, y1)
            c.itemconfigure(self.log_item, state='normal', width=w - px(64) - x1, height=max(px(120), h - px(300) - y1))

    def _layout_settings(self, w, h):
        c = self.canvas
        x1, y1, x2, y2 = px(48), px(104), w - px(48), h - px(36)
        radius = px(14)
        if self.backdrop.available():
            c.create_image(0, 0, image=self.backdrop.settings(w, h, (x1, y1, x2, y2), radius), anchor='nw',
                           tags=('chrome', 'backdrop'))
        else:
            _round_rect(c, x1, y1, x2, y2, radius, fill=PANEL, outline='', tags=('chrome',))
        _round_rect(c, x1, y1, x2, y2, radius, fill='', outline=LINE, width=px(1), tags=('chrome',))
        self._text(px(64), px(58), 'Settings', (DISPLAY, 26), TEXT)
        back = self._text(w - px(64), px(60), '‹  Back', (UI, 11, 'bold'), CYAN, anchor='e', tags=('back',))
        self._text(c.bbox(back)[0] - px(14), px(60), 'Esc · B', (UI, 8), FAINT, anchor='e')
        c.tag_bind('back', '<Button-1>', lambda _e: self.show_home())
        c.tag_bind('back', '<Enter>', lambda _e: c.configure(cursor='hand2'))
        c.tag_bind('back', '<Leave>', lambda _e: c.configure(cursor=''))
        # The group tabs along the panel's top edge.
        x, ty = x1 + px(32), y1 + px(32)
        for group in cfg.GROUPS:
            on = group == self.group
            item = self._text(x, ty, cfg.GROUP_TITLES[group], (UI, 11, 'bold' if on else 'normal'),
                              TEXT if on else MUTED, tags=(f'tab_{group}',))
            bx1, _by1, bx2, _by2 = c.bbox(item)
            if on:
                c.create_line(bx1, ty + px(16), bx2, ty + px(16), fill=GOLD if self.nav_mode else CYAN,
                              width=px(2), tags=('chrome',))
            c.tag_bind(f'tab_{group}', '<Button-1>', lambda _e, g=group: self.show_group(g, enter=True))
            c.tag_bind(f'tab_{group}', '<Enter>', lambda _e: c.configure(cursor='hand2'))
            c.tag_bind(f'tab_{group}', '<Leave>', lambda _e: c.configure(cursor=''))
            x = bx2 + px(30)
        c.create_line(x1 + px(1), ty + px(26), x2 - px(1), ty + px(26), fill=LINE, tags=('chrome',))
        # The rows of the group: label (and its default) on the left, the control right-aligned.
        settings = cfg.group_settings(self.group)
        top, bottom = ty + px(40), y2 - px(104)
        row_h = max(px(40), min(px(54), (bottom - top) // max(1, len(settings))))
        for i, setting in enumerate(settings):
            mid = top + i * row_h + row_h // 2
            if setting.hint:
                self._text(x1 + px(32), mid - px(7), setting.label, (UI, 11), TEXT, tags=(f'row_{setting.key}',))
                self._text(x1 + px(32), mid + px(11), f'Default {setting.hint}', (UI, 8), FAINT)
            else:
                self._text(x1 + px(32), mid, setting.label, (UI, 11), TEXT, tags=(f'row_{setting.key}',))
            c.tag_bind(f'row_{setting.key}', '<Enter>', lambda _e, s=setting: self._show_help(s))
            if i:
                c.create_line(x1 + px(32), mid - row_h // 2, x2 - px(32), mid - row_h // 2, fill=LINE,
                              tags=('chrome',))
            item = self.windows[setting.key]
            c.coords(item, x2 - px(32), mid)
            c.itemconfigure(item, state='normal')
        # Help for the focused (or hovered) setting, then the status and the two panel actions.
        c.create_line(x1 + px(1), y2 - px(92), x2 - px(1), y2 - px(92), fill=LINE, tags=('chrome',))
        self.help_item = self._text(x1 + px(32), y2 - px(66), self.help_text, (UI, 9), MUTED, anchor='w',
                                    width=max(px(200), x2 - x1 - px(64)))
        self.status_item = self._text(x1 + px(32), y2 - px(28), self.status_text, (UI, 9),
                                      RED if self.status_bad else FAINT)
        c.coords(self.save_item, x2 - px(32), y2 - px(28))
        c.itemconfigure(self.save_item, state='normal')
        c.update_idletasks()
        c.coords(self.reset_item, x2 - px(44) - self.save_button.winfo_reqwidth(), y2 - px(28))
        c.itemconfigure(self.reset_item, state='normal')

    # -- views ----------------------------------------------------------------------------------------------------
    def show_home(self):
        self.view, self.nav_mode = 'home', False
        self.focus_set()
        self.layout()

    def show_settings(self):
        self.view = 'settings'
        self.layout()
        self._enter_page()

    def show_group(self, group, enter=False):
        changed = group != self.group or self.view != 'settings'
        self.group, self.view = group, 'settings'
        if changed or not enter:
            self.layout()
        if enter:
            self._enter_page()

    def _escape(self):
        if self.view == 'settings':
            self.show_home()
        else:
            self.destroy()

    def _arrow(self, event, step):
        if self.view == 'home' and not isinstance(event.widget, (tk.Entry, tk.Text)):
            self._menu_select((self.menu_index + step) % len(self._menu()))

    def _home_enter(self, event):
        if self.view == 'home' and event.widget in (self, self.canvas):
            self._menu_activate()

    def _menu_select(self, index):
        if index != self.menu_index:
            self.menu_index = index
            self.layout()

    def _menu(self):
        """The home menu. On Windows with tools/b3_win.py: Set up first, then Start game and Rebuild."""
        if os.name != 'nt' or not (self.root / 'tools/b3_win.py').is_file():
            return self.MENU
        if not cfg.install_state(self.root)['built']:
            return ('setup', 'settings', 'log', 'quit')
        return ('play', 'rebuild', 'settings', 'log', 'quit')

    def _menu_activate(self):
        menu = self._menu()
        self.menu_index = min(self.menu_index, len(menu) - 1)
        key = menu[self.menu_index]
        if key == 'setup':
            self.run_setup()
        elif key == 'rebuild':
            self.run_task('update', ['update'])
        elif key == 'play':
            self.play_or_stop()
        elif key == 'settings':
            self.show_settings()
        elif key == 'log':
            self.toggle_log()
        elif key == 'quit':
            self.destroy()

    # -- values ----------------------------------------------------------------------------------------------------
    def _load_values(self):
        for setting in cfg.SETTINGS:
            value = self.conf.effective(setting)
            var = self.vars[setting.key]
            if setting.kind == 'bool':
                var.set(cfg.is_on(value))
            elif setting.kind == 'choice':
                labels = [label for v, label in setting.choices]
                match = [label for v, label in setting.choices if v == value]
                var.set(match[0] if match else labels[0])
            else:
                var.set(value)

    def _value_of(self, setting):
        if setting.kind == 'bool':
            return '1' if self.vars[setting.key].get() else '0'
        if setting.kind == 'choice':
            label = self.vars[setting.key].get()
            for value, text in setting.choices:
                if text == label:
                    return value
            return ''
        return self.vars[setting.key].get().strip()

    def _values(self):
        return {s.key: self._value_of(s) for s in cfg.SETTINGS}

    def _describe(self):
        return cfg.summary(cfg.Conf.parse(self.conf.render()).apply(self._values()))

    def _dirty(self):
        return any(self._value_of(s) != self.conf.effective(s) for s in cfg.SETTINGS)

    def _ready_text(self):
        return 'Unsaved changes' if self._dirty() else 'All changes saved'

    def _refresh_status(self):
        if not hasattr(self, 'canvas') or self.proc is not None or not self.vars or len(self.vars) < len(cfg.SETTINGS):
            return
        self._set_status(self._ready_text())

    def _short(self, path):
        try:
            return str(path.relative_to(self.root))
        except ValueError:
            return str(path)

    def _set_status(self, text, bad=False):
        if len(text) > 110:
            text = text[:109].rstrip() + '…'
        self.status_text, self.status_bad = text, bad
        if self.view == 'settings' and getattr(self, 'status_item', None):
            self.canvas.itemconfigure(self.status_item, text=text, fill=RED if bad else FAINT)
        elif self.view == 'home' and self.canvas.winfo_ismapped():
            self._schedule_layout()

    def _show_help(self, setting):
        self.help_text = f'{setting.label} — {setting.help}' if setting.help else setting.label
        if self.view == 'settings' and getattr(self, 'help_item', None):
            self.canvas.itemconfigure(self.help_item, text=self.help_text)

    def _run_state(self):
        if self.task is not None:
            return self.status_text
        if self.proc is not None:
            return 'Running' if not self.stopping else 'Stopping…'
        if self.status_bad:
            return self.status_text
        state = cfg.install_state(self.root)
        if not state['built']:
            return f'Not built yet · {self._build_hint()}'
        return self.status_text if self.status_text.startswith(('The game', 'Stopped')) else 'Ready to play'

    def _run_colour(self):
        if self.proc is not None:
            return CYAN
        return GOLD if cfg.install_state(self.root)['built'] and not self.status_bad else RED

    # -- actions ----------------------------------------------------------------------------------------------------
    def save(self, quiet=False):
        values = self._values()
        problems = [(s, m) for s in cfg.SETTINGS if (m := cfg.validate_value(s, values[s.key]))]
        if problems:
            setting, message = problems[0]
            self.show_group(setting.group)
            self._set_status(f'{setting.label}: {message}', bad=True)
            self.rows[setting.key][1].focus_set()
            return False
        self.conf.apply(values)
        self.conf.save(self.paths['conf'])
        if not quiet:
            self._set_status(f'Saved to {self._short(self.paths["conf"])}')
        return True

    def reset(self):
        if not messagebox.askokcancel('Reset settings', 'Drop every saved setting and go back to the defaults?', parent=self):
            return
        cfg.reset_to_defaults(self.conf)
        self.conf.save(self.paths['conf'])
        self._load_values()
        self._set_status('Defaults restored')

    # -- setup and update (Windows: tools/b3_win.py) ---------------------------------------------------------------
    def run_setup(self):
        if self.task is not None:
            return
        dump = filedialog.askdirectory(parent=self, title='Your Bayonetta 3 1.2.0 dump: the folder with exefs and romfs')
        if not dump:
            return
        if not messagebox.askokcancel('Set up', 'This builds the game from your dump on this PC. The first time takes '
                                      'about an hour; if you close the launcher, Set up resumes where it stopped.',
                                      parent=self):
            return
        self.run_task('setup', ['setup', '--dump', dump])

    def run_task(self, kind, args):
        if self.task is not None or self.proc is not None:
            return
        self.task_kind, self.task_log = kind, []
        self.task_step, self.task_done = '', 0
        self.task_total = 7 if kind == 'setup' else 1
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        try:
            self.task = subprocess.Popen([sys.executable, str(self.root / 'tools/b3_win.py'), *args],
                                         cwd=str(self.root), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, encoding='utf-8', errors='replace', creationflags=flags)
        except OSError as exc:
            self._set_status(f'Could not start {kind}: {exc}', bad=True)
            return

        def read(stream=self.task.stdout, lines=self.task_lines):
            for line in stream:
                lines.put(line.rstrip())
        threading.Thread(target=read, daemon=True).start()
        self.log_shown = True
        self._set_status(f'{kind.capitalize()}: starting')
        self.layout()

    def _task_line(self, line):
        """Progress from b3_win.py's lines: `b3: <step>: done in N s` (setup), `b3: update: <step>: ...` (update)."""
        self.task_log = (self.task_log + [line])[-200:]
        m = re.match(r'^b3: (?:update: )?(\w+): (.*)$', line)
        if not m:
            return
        step, rest = m.groups()
        if self.task_kind == 'update' and line.startswith('b3: update: ') and not rest.startswith('done'):
            self.task_total = max(self.task_total, sum(1 for l in self.task_log
                                                       if l.startswith('b3: update: ') and ': done' not in l))
        if rest.startswith('done'):
            self.task_done += 1
        else:
            self.task_step = step

    def _poll_task(self):
        changed = False
        while True:
            try:
                line = self.task_lines.get_nowait()
            except queue.Empty:
                break
            self._task_line(line)
            changed = True
        if changed and self.log_shown:
            self.log_view.configure(state='normal')
            self.log_view.delete('1.0', 'end')
            self.log_view.insert('end', '\n'.join(self.task_log[-80:]))
            self.log_view.see('end')
            self.log_view.configure(state='disabled')
        if self.task is not None and self.task.poll() is not None and self.task_lines.empty():
            code, kind = self.task.returncode, self.task_kind
            self.task = None
            if code == 0:
                self._set_status(f'{kind.capitalize()} finished: ready to play')
            else:
                last = next((l for l in reversed(self.task_log) if 'error' in l.lower()), f'exit code {code}')
                self._set_status(f'{kind.capitalize()} stopped: {last}', bad=True)
            self.menu_index = 0
            self.layout()
        elif changed and self.task is not None:
            self._set_status(f'{self.task_kind.capitalize()}: {self.task_step or "starting"} '
                             f'({self.task_done}/{self.task_total})')
        self.after(300, self._poll_task)

    def play_or_stop(self):
        if self.proc is None:
            self.play()
        else:
            self.stop()

    def play(self):
        if self.proc is not None or self.task is not None:
            return
        state = cfg.install_state(self.root)
        if not state['built']:
            self._set_status(f'Not built yet: {self._short(self.paths["binary"])} ({self._build_hint()})', bad=True)
            return
        if not state['play']:
            self._set_status(f'Entry script missing: {self._short(self.paths["play"])}', bad=True)
            return
        if not self.save(quiet=True):
            return
        self.status_bad = False
        if cfg.is_on(self.conf.get('headless')):
            self._set_status('Headless is on: the game runs without a window (turn it off to play)', bad=True)
        env = cfg.launch_env(self.root, self.conf, base=dict(os.environ))
        argv = cfg.launch_argv(self.root)
        try:
            if os.name == 'nt':
                # Windows starts `tools/b3_win.py play` with this Python (launch_argv). A .sh entry script, if one
                # is configured, needs bash, and `bash` in PATH is usually the WSL launcher stub, which exits with
                # "no installed distributions": a real bash is resolved the way scripts/lib/b3_common.sh does
                # (B3_BASH first, then Git for Windows). No start_new_session: that is POSIX-only.
                if argv[0].endswith('.sh'):
                    argv = [self._host_bash(), *argv]
                self.proc = subprocess.Popen(argv, cwd=str(self.root), env=env, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                self.proc = subprocess.Popen(argv, cwd=str(self.root), env=env, stdout=subprocess.DEVNULL,
                                             stderr=subprocess.STDOUT, start_new_session=True)
        except OSError as exc:
            self._set_status(f'Could not start the game: {exc}', bad=True)
            return
        self._set_status(f'Running · log {self._short(self.paths["log"])}')
        self.layout()

    @staticmethod
    def _build_hint():
        return cfg.build_hint()

    @staticmethod
    def _host_bash():
        """A bash that actually runs, for the .sh entry script.

        On Windows the first `bash` in PATH is usually the WSL launcher stub (C:\\Windows\\system32\\bash.exe), which
        exits with "no installed distributions". Resolving the name therefore proves nothing: each candidate is run
        once and kept only if it prints `ok`. B3_BASH overrides, as in scripts/lib/b3_common.sh.
        """
        import shutil
        explicit = os.environ.get('B3_BASH')
        candidates = []
        if explicit:
            candidates.append(explicit)
        found = shutil.which('bash')
        if found:
            candidates.append(found)
        for base in (os.environ.get('ProgramFiles'), os.environ.get('ProgramFiles(x86)')):
            if base:
                guards = (os.path.join(base, r'Git\bin\bash.exe'), os.path.join(base, r'Git\usr\bin\bash.exe'))
                candidates.extend(p for p in guards if os.path.isfile(p))
        git = shutil.which('git')
        if git:
            # A portable/MinGW git keeps bash beside it, which is where this checkout's toolchain lived.
            git_root = os.path.dirname(os.path.dirname(os.path.abspath(git)))
            for rel in (r'bin\bash.exe', r'usr\bin\bash.exe', r'..\bin\bash.exe', r'..\usr\bin\bash.exe'):
                path = os.path.normpath(os.path.join(git_root, rel))
                if os.path.isfile(path):
                    candidates.append(path)
        candidates.extend(['bash', 'bash.exe'])
        for candidate in candidates:
            try:
                probe = subprocess.run([candidate, '-c', 'echo ok'], capture_output=True, timeout=10,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            except (OSError, subprocess.SubprocessError):
                continue
            if probe.returncode == 0 and b'ok' in probe.stdout:
                return candidate
        return explicit or 'bash'

    def stop(self):
        if self.proc is None:
            return
        self.stopping = True
        try:
            if os.name == 'nt':
                # No process groups to signal: ask the tree to close, and the game's own window close path runs.
                subprocess.run(['taskkill', '/F', '/T', '/PID', str(self.proc.pid)], capture_output=True,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                os.killpg(os.getpgid(self.proc.pid), 15)   # SIGTERM to the whole group: the handoff child too
        except OSError:
            pass
        self._set_status('Stopping…')

    def toggle_log(self):
        self.log_shown = not self.log_shown
        if self.log_shown:
            self._poll_log_once()
        self.layout()

    # -- timers -----------------------------------------------------------------------------------------------------
    def _poll_proc(self):
        if self.proc is not None and self.proc.poll() is not None:
            code = self.proc.returncode
            was_stop = self.stopping
            self.proc, self.stopping = None, False
            self._set_status(self._exit_text(code, was_stop))
            self.layout()
        self.after(400, self._poll_proc)

    @staticmethod
    def _exit_text(code, was_stop):
        # The game's own exit is a signal when the window closes (86 relaunches the ExecuteProgram handoff, which
        # scripts/run_linux.sh loops), so translate rather than show a raw negative number to the player. On Windows
        # a killed process reports 1, not a negative signal, because there are no POSIX signals to report.
        if code == 0:
            return 'The game exited'
        if code == 86:
            return 'The game handed off to the next program (code 86)'
        if was_stop and (code < 0 or (code == 1 and os.name == 'nt')):
            return 'Stopped'
        if code < 0:
            return f'The game was killed (signal {-code})'
        return f'The game exited (code {code})'

    def _poll_log_once(self):
        try:
            if not self.paths['log'].exists():
                text = ['(no log yet: it appears once the game has run)']
            else:
                text = self.paths['log'].read_text(encoding='utf-8', errors='replace').splitlines()[-80:]
            self.log_view.configure(state='normal')
            self.log_view.delete('1.0', 'end')
            self.log_view.insert('end', '\n'.join(text) or '(empty log)')
            self.log_view.see('end')
            self.log_view.configure(state='disabled')
        except OSError:
            pass

    def _poll_log(self):
        if self.log_shown:
            self._poll_log_once()
        self.after(1500, self._poll_log)

    def _poll_pad(self):
        self.pad_tick += 1
        if self.pad_tick % 20 == 0:
            before = self._pad_text()
            self.pad.refresh()
            if self.view == 'home' and self._pad_text() != before:
                self.layout()
        for action in self.pad.pressed():
            self._pad_action(action)
        self.after(60, self._poll_pad)

    def _pad_text(self):
        if self.pad.error:
            return f'Controller: {self.pad.error}'
        if self.pad.names:
            return self.pad.names[0]
        if self.pad.fallback is not None:
            # XInput is live and no pad is plugged in: say which path is ready, so a player who later connects one
            # knows it will be picked up (the poll loop re-scans every 1.2 s).
            return 'No controller yet · XInput ready'
        return 'No controller · keyboard works'

    def _pad_colour(self):
        return RED if self.pad.error else (CYAN if self.pad.names else FAINT)

    # -- navigation -------------------------------------------------------------------------------------------------
    # Home: up/down choose a menu entry, A (or Start on Start game) takes it. Settings: up/down move through the
    # group's settings, left/right step a list or number, A flips or steps, B goes up to the tabs (where left/right
    # change group and A comes back down), and B on the tabs returns home. Start always starts the game.
    def _pad_action(self, action):
        if action == 'start':
            self.play()
            return
        if self.view == 'home':
            if action in ('up', 'down'):
                self._menu_select((self.menu_index + (-1 if action == 'up' else 1)) % len(self._menu()))
            elif action == 'a':
                self._menu_activate()
            return
        if self.nav_mode:
            if action in ('left', 'right', 'up', 'down'):
                index = cfg.GROUPS.index(self.group) + (-1 if action in ('left', 'up') else 1)
                self.group = cfg.GROUPS[index % len(cfg.GROUPS)]
                self.layout()
            elif action == 'a':
                self._enter_page()
            elif action == 'b':
                self.show_home()
            return
        if action in ('up', 'down', 'left', 'right'):
            self._move_focus(-1 if action == 'up' else 1 if action == 'down' else 0,
                             -1 if action == 'left' else 1 if action == 'right' else 0)
        elif action == 'a':
            self._accept()
        elif action == 'b':
            self.nav_mode = True
            self.focus_set()
            self.layout()

    def _page_widgets(self):
        return [self.rows[s.key][1] for s in cfg.group_settings(self.group)]

    def _focus_moved(self, widget):
        self.nav_mode = False
        if widget in self.focusable:
            self.focus_index = self.focusable.index(widget)
        setting = self._setting_of(widget)
        if setting is not None:
            self._show_help(setting)

    def _move_focus(self, vertical, horizontal):
        page = self._page_widgets()
        if not page:
            return
        current = self.focusable[self.focus_index] if self.focus_index < len(self.focusable) else page[0]
        index = page.index(current) if current in page else 0
        if horizontal:
            setting = self._setting_of(current)
            if setting is not None and setting.kind in ('choice', 'int'):
                self._nudge(setting, horizontal)
            return
        widget = page[(index + vertical) % len(page)]
        self.focus_index = self.focusable.index(widget)
        widget.focus_set()
        setting = self._setting_of(widget)
        if setting is not None:
            self._show_help(setting)

    def _enter_page(self):
        was_nav = self.nav_mode
        self.nav_mode = False
        page = self._page_widgets()
        if page:
            widget = page[0]
            self.focus_index = self.focusable.index(widget)
            self._show_help(self._setting_of(widget))
            widget.focus_set()
        if was_nav:
            self.layout()

    def _setting_of(self, widget):
        for setting in cfg.SETTINGS:
            if self.rows[setting.key][1] is widget:
                return setting
        return None

    def _accept(self):
        widget = self.focusable[self.focus_index] if self.focusable else None
        setting = self._setting_of(widget) if widget is not None else None
        if setting is None:
            return
        if setting.kind == 'bool':
            self.vars[setting.key].set(not self.vars[setting.key].get())
        elif setting.kind == 'choice':
            self._nudge(setting, 1)
        self._refresh_status()

    def _nudge(self, setting, direction):
        if setting.kind == 'choice':
            self.rows[setting.key][1].step(direction)
        elif setting.kind == 'int':
            step = 500 if setting.hi > 1000 else 1
            try:
                value = int(self.vars[setting.key].get() or setting.lo)
            except ValueError:
                value = setting.lo
            value = min(setting.hi, max(setting.lo, value + direction * step))
            self.vars[setting.key].set(str(value))
        self._refresh_status()


def main(argv):
    if not os.environ.get('DISPLAY') and not os.environ.get('WAYLAND_DISPLAY') and sys.platform.startswith('linux'):
        print('b3 launcher: no display; use scripts/b3_play.sh or edit private/b3.conf directly', file=sys.stderr)
        return 2
    if len(argv) > 1 and argv[1] in ('--check', 'check'):
        # Headless self-test: the paths and the launch plan without opening a window. Windows is the host that made
        # this necessary -- there is no X11/Wayland session variable to test for, and a CI run has no desktop.
        root = cfg.repo_root()
        p = cfg.paths(root)
        state = cfg.install_state(root)
        print(f'host: {cfg.host_platform()}')
        for key in ('conf', 'template', 'play', 'binary', 'data', 'log'):
            print(f'{key}: {p[key]}')
        print('state: ' + ' '.join(f'{k}={"yes" if v else "no"}' for k, v in state.items()))
        print('argv: ' + ' '.join(str(a) for a in cfg.launch_argv(root)))
        print('build hint: ' + cfg.build_hint())
        print('background art: ' + ('yes' if Backdrop().available() else 'no (needs Pillow and the asset)'))
        return 0 if state['play'] else 1
    _dpi_aware()
    app = Launcher()
    if len(argv) > 1 and argv[1] in ('--settings', 'settings'):
        app.after(100, app.show_settings)
    if len(argv) > 1 and argv[1] in ('--play', 'play'):
        app.after(300, app.play)
    app.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
