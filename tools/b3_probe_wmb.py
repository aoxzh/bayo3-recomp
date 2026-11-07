#!/usr/bin/env python3
"""Bounded RC11 WMB diagnosis. Launch only under the benchmark owner's control.

As EXE: no arguments; NX_DATA_ROOT comes from b3_experiment.sh.
Create B3_WMB_ARM_FILE near death/retry to install the four narrow probes.
No game is launched by --check or --self-test. FPS under this wrapper is invalid.
"""
import ast
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import signal
import struct
import subprocess
import sys
import threading
import time
from collections import deque

ROOT = Path(__file__).resolve().parents[1]
RC11 = ROOT / "private/bin/b3-rc11"
RC11_MD5 = "5f8121fec577b7c0f2c91498523bd433"
PREFIX = "B3_WMB: "


def note(text):
    print(PREFIX + text, flush=True)


def proc_identity(pid):
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
        return text[text.rfind(")") + 2:].split()[19]
    except (OSError, IndexError):
        return None


def check_binary():
    with RC11.open("rb") as stream:
        digest = hashlib.file_digest(stream, "md5").hexdigest()
    if digest != RC11_MD5:
        raise RuntimeError(f"RC11 hash mismatch: {digest}")
    for path in Path("/proc").glob("[0-9]*/exe"):
        try:
            if path.resolve() == RC11.resolve():
                raise RuntimeError(f"RC11 already running: PID {path.parent.name}")
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
    return digest


def gdb_install():
    import gdb

    history = deque(maxlen=32)
    active = {}
    probes = []
    output = os.environ.get("B3_WMB_OUTPUT", "")
    dumped = False
    last_cpu = None
    probe_error = None

    def mem(address, size):
        if size < 0 or size > 4096:
            raise ValueError("probe read exceeds 4096 bytes")
        return bytes(gdb.selected_inferior().read_memory(address, size))

    def number(address, size=8):
        return int.from_bytes(mem(address, size), "little")

    def regs(cpu):
        return list(struct.unpack("<32Q", mem(cpu, 256)))

    def safe_memory(address, size):
        try:
            return {"address": hex(address), "hex": mem(address, size).hex()}
        except Exception as exc:
            return {"address": hex(address), "error": str(exc)}

    def containers(address, detail=False):
        result = {"address": hex(address), "slots": []}
        for index in range(2):
            slot = {"index": index}
            result["slots"].append(slot)
            try:
                base = number(address + 8 * index)
                slot["base"] = hex(base)
                if not base:
                    continue
                header = mem(base, 64)
                slot["header_hex"] = header.hex()
                count, offsets, extensions, names = struct.unpack_from("<4I", header, 4)
                slot.update(count=count, offsets_offset=offsets,
                            extensions_offset=extensions, names_offset=names)
                if not detail:
                    continue
                kept = min(count, 128)
                slot["entries_truncated"] = count > kept
                slot["entries"] = []
                stride = number(base + names, 4) if names else 0
                slot["name_stride"] = stride
                for item in range(kept):
                    entry = {"index": item}
                    slot["entries"].append(entry)
                    if offsets:
                        entry["offset"] = number(base + offsets + item * 4, 4)
                    if extensions:
                        entry["extension_hex"] = mem(base + extensions + item * 4, 4).hex()
                    if names and 0 < stride <= 4096:
                        raw = mem(base + names + 4 + item * stride, min(stride, 128))
                        entry["name"] = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
            except Exception as exc:
                slot["error"] = str(exc)
        return result

    def emit(payload, suffix=""):
        target = output + suffix
        with open(target, "x", encoding="utf-8") as stream:
            json.dump(payload, stream, indent=2)
            stream.write("\n")
        note("snapshot written " + target)

    class Probe(gdb.Breakpoint):
        def __init__(self, address, kind):
            super().__init__(f"*{address:#x}", internal=True)
            self.kind = kind
            self.silent = True

        def stop(self):
            nonlocal dumped, last_cpu, probe_error
            if dumped:
                return False
            try:
                cpu = int(gdb.parse_and_eval("$rdi" if self.kind == "null_constructor" else "$rbx"))
                x = regs(cpu)
                key = (cpu, x[31])
                if self.kind == "null_constructor":
                    if x[0] or x[30] != 0x400064241C:
                        return False
                    dumped = True
                    last_cpu = cpu
                    payload = {
                        "kind": "null_wmb_constructor", "diagnostic_only": True,
                        "binary": str(RC11), "expected_md5": RC11_MD5,
                        "cpu": hex(cpu), "guest_x": [hex(v) for v in x],
                        "thread": list(gdb.selected_thread().ptid),
                        "container": containers(x[31] + 8, True),
                        "caller_stack": safe_memory(x[31], 176),
                        "history": list(history),
                        "backtrace": gdb.execute("bt 24", to_string=True),
                        "native_registers": gdb.execute("info registers", to_string=True),
                        "limits": "Last 32 model loads; at most 128 entries per container and 128 name bytes. No memory or register modification."
                    }
                    emit(payload)
                    # Stop once, then observe the actual fault without delivering its signal.
                    return True
                if self.kind == "pre_loader":
                    item = {"cpu": hex(cpu), "sp": hex(x[31]),
                            "thread": list(gdb.selected_thread().ptid),
                            "request_id": hex(x[2]),
                            "original_id": hex(number(x[31] + 0x24, 4)),
                            "pre_x": [hex(v) for v in x]}
                    history.append(item)
                    active[key] = item
                    # Bound stale entries if a loader unwinds or never reaches the lookup.
                    if len(active) > 64:
                        active.pop(next(iter(active)))
                elif self.kind == "post_loader":
                    item = active.get(key)
                    if item is not None:
                        item["loader_result"] = x[0]
                        item["container"] = containers(x[31] + 8)
                elif self.kind == "post_wmb":
                    item = active.pop(key, None)
                    if item is not None:
                        item["wmb_result"] = hex(x[0])
                return False
            except Exception as exc:
                probe_error = {"kind": "probe_error", "location": self.kind, "error": str(exc)}
                note(f"probe error at {self.kind}: {exc}")
                try:
                    emit(probe_error, ".error.json")
                except Exception as write_error:
                    note("error snapshot could not be written: " + str(write_error))
                return True

    class Arm(gdb.Command):
        def __init__(self):
            super().__init__("b3-wmb-arm", gdb.COMMAND_USER)

        def invoke(self, argument, from_tty):
            if probes:
                raise gdb.GdbError("WMB probes already armed")
            if not output or Path(output).exists():
                raise gdb.GdbError("B3_WMB_OUTPUT must name a new file")
            base = int(gdb.parse_and_eval("(void*)&main_006421D4"))
            # Verify both native direct-call destinations after PIE relocation.
            for offset, target in [(0x6F3, "main_0081D200"), (0x72A, "main_00CE42E4")]:
                raw = mem(base + offset, 5)
                actual = base + offset + 5 + int.from_bytes(raw[1:], "little", signed=True)
                expected = int(gdb.parse_and_eval(f"(void*)&{target}"))
                if raw[0] != 0xE8 or actual != expected:
                    raise gdb.GdbError(f"RC11 instruction mismatch at +{offset:#x}")
            for offset, kind in [(0x6F3, "pre_loader"), (0x6F8, "post_loader"), (0x72F, "post_wmb")]:
                probes.append(Probe(base + offset, kind))
            address = int(gdb.parse_and_eval("(void*)&main_00808890"))
            probes.append(Probe(address, "null_constructor"))
            note(f"armed 4 narrow probes; caller base {base:#x}")

    class Fault(gdb.Command):
        def __init__(self):
            super().__init__("b3-wmb-fault", gdb.COMMAND_USER)

        def invoke(self, argument, from_tty):
            payload = {
                "kind": "observed_fault", "diagnostic_only": True,
                "null_wmb_observed": dumped, "binary": str(RC11),
                "inferior_executable": gdb.current_progspace().filename,
                "native_pc": str(gdb.parse_and_eval("$pc")),
                "native_symbol": gdb.execute("info symbol $pc", to_string=True),
                "siginfo": gdb.execute("p $_siginfo", to_string=True),
                "backtrace": gdb.execute("bt 24", to_string=True),
                "native_registers": gdb.execute("info registers", to_string=True),
                "null_snapshot_cpu": hex(last_cpu) if last_cpu else None,
            }
            if last_cpu:
                try:
                    payload["guest_x_at_fault"] = [hex(v) for v in regs(last_cpu)]
                except Exception as exc:
                    payload["guest_x_error"] = str(exc)
            emit(payload, ".fault.json")

    class Check(gdb.Command):
        def __init__(self):
            super().__init__("b3-wmb-check", gdb.COMMAND_USER)

        def invoke(self, argument, from_tty):
            if probe_error is not None:
                raise gdb.GdbError("probe aborted: " + json.dumps(probe_error))
            if not dumped:
                raise gdb.GdbError("unexpected probe stop without a null snapshot")

    Arm()
    Fault()
    Check()
    note("probe command loaded; no breakpoints installed before arm")


class Session:
    """Small MI supervisor; GDB remains the inferior's parent (ptrace_scope=1)."""
    def __init__(self, executable, arguments=(), load_probe=True):
        self.events = queue.Queue()
        self.inferior = None
        self.inferior_start = None
        self.exited = False
        self.process = subprocess.Popen(
            ["gdb", "-q", "-nx", "--interpreter=mi2", "--args", str(executable), *arguments],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.token = 0
        self.stops = deque()
        try:
            for command in ["-gdb-set pagination off", "-gdb-set confirm off",
                            "-gdb-set startup-with-shell off", "-gdb-set mi-async on"]:
                self.request(command)
            self.console("handle SIGSEGV stop print nopass")
            self.console("handle SIGABRT stop print nopass")
            if load_probe:
                self.console("source " + str(Path(__file__).resolve()))
            self.request("-exec-run")
        except Exception:
            self.close()
            raise

    def _read(self):
        for line in self.process.stdout:
            self.events.put(line.rstrip("\n"))
        self.events.put(None)

    def send(self, command):
        self.token += 1
        self.process.stdin.write(f"{self.token}{command}\n")
        self.process.stdin.flush()
        return self.token

    def receive(self, timeout=0.2):
        try:
            line = self.events.get(timeout=timeout)
        except queue.Empty:
            return ""
        if line is None:
            self.exited = True
            return ""
        match = re.match(r'=thread-group-started,.*pid="(\d+)"', line)
        if match:
            self.inferior = int(match.group(1))
            self.inferior_start = proc_identity(self.inferior)
            note(f"inferior PID {self.inferior}; GDB PID {self.process.pid}")
        if line.startswith('=thread-group-exited') or line.startswith('*stopped,reason="exited'):
            self.exited = True
        if line[:1] in ("~", "@", "&"):
            try:
                sys.stdout.write(ast.literal_eval(line[1:]))
                sys.stdout.flush()
            except (SyntaxError, ValueError):
                print(line, flush=True)
        elif not re.match(r"^(?:\d+\^|[=*+]|\(gdb\))", line):
            print(line, flush=True)
        return line

    def request(self, command):
        token = self.send(command)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            line = self.receive()
            if line.startswith("*stopped"):
                self.stops.append(line)
            if line.startswith(f"{token}^error"):
                raise RuntimeError(line)
            if line.startswith(f"{token}^"):
                return line
            if self.exited:
                raise RuntimeError("GDB/inferior exited during command")
        raise RuntimeError(f"GDB command timed out: {command}")

    def console(self, text):
        return self.request("-interpreter-exec console " + json.dumps(text))

    def close(self):
        try:
            if self.process.poll() is None:
                self.send("-gdb-exit")
                self.process.wait(timeout=3)
        except (OSError, subprocess.TimeoutExpired):
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        finally:
            # Identity checked fallback if GDB failed to reap its original inferior.
            if self.inferior and self.inferior_start and proc_identity(self.inferior) == self.inferior_start:
                try:
                    os.kill(self.inferior, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            self.reader.join(timeout=1)


def self_test():
    # Uses a standard system utility, never the game or generated guest code.
    test_output = ROOT / "logs/ch1" / f"lead30-crash-selftest-{os.getpid()}.json"
    old_output = os.environ.get("B3_WMB_OUTPUT")
    os.environ["B3_WMB_OUTPUT"] = str(test_output)
    session = Session("/usr/bin/sleep", ["30"], load_probe=True)
    try:
        session.console("help b3-wmb-arm")
        session.console("help b3-wmb-fault")
        session.request("-exec-interrupt")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            line = session.stops.popleft() if session.stops else session.receive()
            if line.startswith("*stopped"):
                session.console("python print('B3_WMB_MI_ARM_TEST_OK')")
                session.request("-exec-continue")
                break
        else:
            raise RuntimeError("MI interrupt did not stop the test child")
        pid, identity = session.inferior, session.inferior_start
        os.kill(pid, signal.SIGSEGV)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            line = session.stops.popleft() if session.stops else session.receive()
            if line.startswith("*stopped") and 'signal-name="SIGSEGV"' in line:
                session.console("b3-wmb-fault")
                break
        else:
            raise RuntimeError("MI did not retain the test child at SIGSEGV")
    finally:
        session.close()
        if old_output is None:
            os.environ.pop("B3_WMB_OUTPUT", None)
        else:
            os.environ["B3_WMB_OUTPUT"] = old_output
    payload = json.loads(Path(str(test_output) + ".fault.json").read_text())
    if payload["null_wmb_observed"] or payload["inferior_executable"] != "/usr/bin/sleep":
        raise RuntimeError("incorrect non-game fault payload")
    if not payload.get("native_pc") or "si_signo = 11" not in payload.get("siginfo", ""):
        raise RuntimeError("missing non-game fault context")
    deadline = time.monotonic() + 2
    while proc_identity(pid) == identity and time.monotonic() < deadline:
        time.sleep(0.02)
    if proc_identity(pid) == identity:
        raise RuntimeError("test inferior survived cleanup")
    note("PASS non-game MI launch, interrupt, console, resume, SIGSEGV capture, and stopped-child cleanup")


def launch():
    digest = check_binary()
    arm = Path(os.environ["B3_WMB_ARM_FILE"]).resolve()
    output = Path(os.environ["B3_WMB_OUTPUT"]).resolve()
    if (arm.exists() or output.exists() or Path(str(output) + ".fault.json").exists()
            or Path(str(output) + ".error.json").exists()):
        raise RuntimeError("arm and all output paths must be new")
    if not os.environ.get("NX_DATA_ROOT"):
        raise RuntimeError("NX_DATA_ROOT is required")
    limit = int(os.environ.get("B3_WMB_TIMEOUT", "1100"))
    if not 30 <= limit <= 1800:
        raise RuntimeError("B3_WMB_TIMEOUT must be 30..1800 seconds")
    stopped = False

    def stop_handler(signum, frame):
        nonlocal stopped
        stopped = True

    for signum in (signal.SIGTERM, signal.SIGINT):
        signal.signal(signum, stop_handler)
    note(f"diagnostic only; binary {RC11}; md5 {digest}; arm {arm}; output {output}")
    session = None
    try:
        session = Session(RC11)
        start = time.monotonic()
        arming = False
        armed = False
        while not stopped and not session.exited and time.monotonic() - start < limit:
            line = session.stops.popleft() if session.stops else session.receive()
            if (arm.exists() and not arming and not armed and not session.exited
                    and not line.startswith("*stopped")):
                session.request("-exec-interrupt")
                arming = True
            if line.startswith("*stopped") and not session.exited:
                if 'signal-name="SIGSEGV"' in line or 'signal-name="SIGABRT"' in line:
                    session.console("b3-wmb-fault")
                    note("observed fault captured; terminating stopped inferior without signal delivery")
                    break
                if arming:
                    session.console("b3-wmb-arm")
                    arming = False
                    armed = True
                elif 'reason="breakpoint-hit"' not in line:
                    raise RuntimeError("unexpected inferior stop: " + line)
                else:
                    session.console("b3-wmb-check")
                session.request("-exec-continue")
        note(f"supervisor finished; armed={armed}; snapshot={output.exists()}; inferior_exited={session.exited}")
    finally:
        if session is not None:
            session.close()


try:
    import gdb
except ImportError:
    gdb = None

if gdb is not None:
    gdb_install()
elif __name__ == "__main__":
    try:
        if sys.argv[1:] == ["--check"]:
            note("PASS pinned RC11 md5 " + check_binary())
        elif sys.argv[1:] == ["--self-test"]:
            self_test()
        elif len(sys.argv) == 1:
            launch()
        else:
            raise RuntimeError("usage: b3_probe_wmb.py [--check|--self-test]")
    except Exception as exc:
        note("ERROR " + str(exc))
        sys.exit(1)
