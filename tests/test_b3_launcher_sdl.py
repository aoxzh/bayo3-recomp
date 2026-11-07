#!/usr/bin/env python3
"""Headless SDL controller lifecycle tests for the launcher."""
import ctypes
import os
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
try:
    import b3_launcher as launcher  # noqa: E402
except ModuleNotFoundError as exc:
    if exc.name not in ('tkinter', '_tkinter'):
        raise
    launcher = None


class Function:
    """A ctypes-like callable whose argtypes and restype can be configured."""
    def __init__(self, fn):
        self.fn = fn
        self.argtypes = None
        self.restype = None

    def __call__(self, *args):
        return self.fn(*args)


class FakeSDL:
    def __init__(self):
        self.ids = []
        self.connected = set()
        self.names = {}
        self.buttons = {}
        self.axes = {}
        self.handles = {}  # handle -> instance ID; separate from the SDL open reference count
        self.refs = {}
        self.next_handle = 1000
        self.arrays = {}
        self.allocations = 0
        self.frees = 0
        self.pumps = 0
        self.opens = []
        self.closes = []
        self.quits = 0
        self.init_ok = True
        self.scan_error = False
        self.open_error = set()
        self.disconnect_on_open = set()
        self.name_error = set()
        self.pending_hotplug = None
        self.error = b'fake SDL problem'
        for name, method in {
            'SDL_InitSubSystem': self.init,
            'SDL_QuitSubSystem': self.quit,
            'SDL_GetError': lambda: self.error,
            'SDL_GetGamepads': self.enumerate,
            'SDL_free': self.free,
            'SDL_OpenGamepad': self.open,
            'SDL_CloseGamepad': self.close,
            'SDL_GamepadConnected': self.is_connected,
            'SDL_GetGamepadName': self.name,
            'SDL_GetGamepadButton': self.button,
            'SDL_GetGamepadAxis': self.axis,
            'SDL_PumpEvents': self.pump,
        }.items():
            setattr(self, name, Function(method))

    def init(self, flags):
        assert flags == 0x2000
        return self.init_ok

    def quit(self, flags):
        assert flags == 0x2000
        self.quits += 1

    def enumerate(self, count):
        if self.scan_error:
            return None
        count._obj.value = len(self.ids)
        array = (ctypes.c_uint32 * (len(self.ids) + 1))(*self.ids, 0)
        ptr = ctypes.cast(array, ctypes.POINTER(ctypes.c_uint32))
        self.arrays[ctypes.addressof(array)] = array
        self.allocations += 1
        return ptr

    def free(self, ptr):
        address = ctypes.cast(ptr, ctypes.c_void_p).value
        assert address in self.arrays, 'double free or unknown SDL array'
        del self.arrays[address]
        self.frees += 1

    def open(self, instance_id):
        self.opens.append(instance_id)
        if instance_id in self.open_error or instance_id not in self.connected:
            return None
        # SDL may return the same pointer and increment its reference count.
        handle = next((h for h, i in self.handles.items() if i == instance_id), None)
        if handle is None:
            handle = self.next_handle
            self.next_handle += 1
            self.handles[handle] = instance_id
        self.refs[handle] = self.refs.get(handle, 0) + 1
        if instance_id in self.disconnect_on_open:
            self.connected.discard(instance_id)
        return handle

    def close(self, handle):
        assert self.refs.get(handle, 0) > 0, 'gamepad closed twice'
        self.closes.append(self.handles[handle])
        self.refs[handle] -= 1
        if not self.refs[handle]:
            del self.refs[handle]
            del self.handles[handle]

    def is_connected(self, handle):
        return self.handles[handle] in self.connected

    def name(self, handle):
        instance_id = self.handles[handle]
        if instance_id in self.name_error:
            raise RuntimeError('simulated name lookup failure')
        return self.names.get(instance_id, b'Same name')

    def button(self, handle, code):
        return code in self.buttons.get(self.handles[handle], set())

    def axis(self, handle, axis):
        return self.axes.get((self.handles[handle], axis), 0)

    def pump(self):
        self.pumps += 1
        if self.pending_hotplug is not None:
            self.ids = list(self.pending_hotplug)
            self.connected = set(self.pending_hotplug)
            self.pending_hotplug = None

    def devices(self, *ids):
        self.ids = list(ids)
        self.connected = set(ids)


@unittest.skipIf(launcher is None, 'Python tkinter support is unavailable')
class LauncherSDLTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeSDL()
        self.env = patch.dict(os.environ, {'B3_LAUNCHER_NO_PAD': ''})
        self.env.start()
        self.loader = patch.object(launcher.ctypes, 'CDLL', return_value=self.fake)
        self.loader.start()
        # These are SDL lifecycle tests; do not consult a host's real XInput DLL
        # when simulating SDL errors on Windows.
        fallback = patch.object(launcher.Pad, '_fallback_to_xinput',
                                lambda pad, reason: setattr(pad, 'error', reason))
        fallback.start()
        self.addCleanup(fallback.stop)
        self.pad = None

    def tearDown(self):
        if self.pad:
            self.pad.close()
        self.loader.stop()
        self.env.stop()
        self.assertEqual(self.fake.allocations, self.fake.frees)
        self.assertFalse(self.fake.arrays)
        self.assertFalse(self.fake.refs)

    def make_pad(self):
        self.pad = launcher.Pad()
        return self.pad

    def test_empty_pumps_and_hotplug_after_start(self):
        pad = self.make_pad()
        self.assertEqual(pad.ids, [])
        self.assertEqual(self.fake.frees, 1)  # empty SDL allocation was still freed
        self.fake.pending_hotplug = [91]
        before = self.fake.pumps
        self.assertEqual(pad.pressed(), set())
        self.assertEqual(self.fake.pumps, before + 1)
        pad.refresh()  # the launcher's periodic rescan
        self.assertEqual(pad.ids, [91])
        self.fake.buttons[91] = {0}
        self.assertEqual(pad.pressed(), {'a'})
        self.assertEqual(pad.pressed(), set())

    def test_repeated_scans_retain_exactly_one_reference(self):
        self.fake.devices(1, 2)
        pad = self.make_pad()
        self.fake.buttons[1] = {0}
        self.assertEqual(pad.pressed(), {'a'})
        for _ in range(50):
            pad.refresh()
        self.assertEqual(self.fake.opens, [1, 2])
        self.assertEqual(list(self.fake.refs.values()), [1, 1])
        self.assertEqual(pad.pressed(), set())
        self.assertEqual(self.fake.allocations, 51)

    def test_same_name_replacement_closes_disconnected_and_resets_buttons(self):
        self.fake.devices(1)
        pad = self.make_pad()
        self.fake.buttons[1] = {0}
        self.assertEqual(pad.pressed(), {'a'})
        self.fake.devices(2)  # same name, different joystick instance
        self.fake.buttons[2] = {0}
        self.assertEqual(pad.pressed(), {'a'})  # disconnect triggers immediate refresh
        self.assertEqual(pad.ids, [2])
        self.assertEqual(pad.names, ['Same name'])
        self.assertEqual(self.fake.closes, [1])
        self.assertEqual(list(self.fake.refs.values()), [1])

    def test_reorder_and_secondary_change_obey_primary_and_held_state(self):
        self.fake.devices(1, 2)
        pad = self.make_pad()
        self.fake.buttons[1] = {0}
        self.fake.buttons[2] = {0}
        self.assertEqual(pad.pressed(), {'a'})
        self.fake.devices(1, 3)
        pad.refresh()
        self.assertEqual(pad.ids, [1, 3])
        self.assertEqual(pad.pressed(), set())
        self.assertEqual(self.fake.closes, [2])
        self.fake.devices(3, 1)
        self.fake.buttons[3] = {0}
        pad.refresh()
        self.assertEqual(pad.ids, [3, 1])
        self.assertEqual(pad.pressed(), {'a'})
        self.assertEqual(self.fake.opens, [1, 2, 3])

    def test_empty_scan_closes_all_and_close_is_idempotent(self):
        self.fake.devices(1, 2)
        pad = self.make_pad()
        self.fake.devices()
        pad.refresh()
        self.assertEqual(pad.ids, [])
        self.assertCountEqual(self.fake.closes, [1, 2])
        pad.close()
        pad.close()
        self.assertEqual(self.fake.quits, 1)
        self.assertEqual(pad.pressed(), set())

    def test_duplicate_ids_and_disconnect_during_open(self):
        self.fake.devices(1, 1, 2)
        self.fake.disconnect_on_open.add(2)
        pad = self.make_pad()
        self.assertEqual(pad.ids, [1])
        self.assertEqual(self.fake.opens, [1, 2])
        self.assertEqual(self.fake.closes, [2])
        self.assertEqual(list(self.fake.refs.values()), [1])

    def test_scan_failure_keeps_connected_and_closes_disconnected(self):
        self.fake.devices(1, 2)
        pad = self.make_pad()
        self.fake.connected.remove(1)
        self.fake.scan_error = True
        pad.refresh()
        self.assertEqual(pad.ids, [2])
        self.assertIn('scan failed', pad.error)
        self.assertIn('fake SDL problem', launcher.Launcher._pad_text(SimpleNamespace(pad=pad)))
        self.assertEqual(self.fake.closes, [1])
        self.fake.scan_error = False
        self.fake.devices(2)
        pad.refresh()
        self.assertEqual(pad.error, '')

    def test_init_and_open_errors_are_visible_and_balanced(self):
        self.fake.init_ok = False
        pad = self.make_pad()
        self.assertIn('initialization failed', launcher.Launcher._pad_text(SimpleNamespace(pad=pad)))
        self.assertIn('fake SDL problem', pad.error)
        self.assertEqual(pad.pressed(), set())
        pad.close()
        self.assertEqual(self.fake.quits, 0)
        self.fake.init_ok = True
        self.fake.devices(3)
        self.fake.open_error.add(3)
        pad = self.make_pad()
        self.assertIn('Could not open controller 3', launcher.Launcher._pad_text(SimpleNamespace(pad=pad)))
        self.fake.open_error.clear()
        pad.refresh()
        self.assertEqual(pad.ids, [3])
        self.assertEqual(pad.error, '')

    def test_library_error_is_visible(self):
        self.loader.stop()
        self.loader = patch.object(launcher.ctypes, 'CDLL', side_effect=OSError('missing SDL'))
        self.loader.start()
        pad = self.make_pad()
        self.assertIn('missing SDL', launcher.Launcher._pad_text(SimpleNamespace(pad=pad)))
        self.assertIsNone(pad.lib)

    def test_exception_releases_new_handles_and_keeps_old(self):
        self.fake.devices(1)
        pad = self.make_pad()
        self.fake.devices(1, 2, 3)
        self.fake.name_error.add(3)
        with self.assertRaises(RuntimeError):
            pad.refresh()
        self.assertEqual(pad.ids, [1])
        self.assertCountEqual(self.fake.closes, [2, 3])
        self.assertEqual(list(self.fake.refs.values()), [1])

    def test_window_destroy_closes_pad(self):
        self.fake.devices(1)
        pad = self.make_pad()
        window = launcher.Launcher.__new__(launcher.Launcher)
        object.__setattr__(window, 'pad', pad)
        with patch.object(launcher.tk.Tk, 'destroy') as destroy:
            launcher.Launcher.destroy(window)
            destroy.assert_called_once_with()
        self.assertEqual(self.fake.closes, [1])
        self.assertEqual(self.fake.quits, 1)


if __name__ == '__main__':
    unittest.main()
