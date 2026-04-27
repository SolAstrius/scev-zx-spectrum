"""
RVVM Speccy test harness.

Spawns the RVVM emulator with our firmware, talks to its UART over
stdin/stdout. We don't bother with the bochs window or HID — the
firmware's debug surface gives us everything we need:

    `v   - 24×32 ASCII screenshot of the screen
    `m X - dump 256 bytes of memory from hex address X
    `d   - Z80 register dump
    `k   - keyboard matrix + sysvars
    plain chars - typed as Speccy keystrokes (queued, debounced)

Typical use:

    from harness import Harness
    h = Harness().start()
    h.wait_boot()
    print(h.screenshot())
    h.type("p")
    h.wait_settle()
    print(h.screenshot())
    h.stop()

The harness assumes the firmware has already been built (firmware.bin
in the parent directory) and that RVVM is at the standard path. Both
are overridable.
"""

from __future__ import annotations

import os
import re
import select
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_RVVM = "/home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64"
DEFAULT_FIRMWARE = Path(__file__).resolve().parent.parent / "firmware.bin"


class Harness:
    def __init__(
        self,
        rvvm: str = DEFAULT_RVVM,
        firmware: str | Path = DEFAULT_FIRMWARE,
        extra_args: list[str] | None = None,
    ) -> None:
        self.rvvm = rvvm
        self.firmware = str(firmware)
        # Headless by default: nogui + nonet + hda_test to make HDA show
        # up on the PCI bus even though we don't render the framebuffer.
        # Caller can override with extra_args = ["-bochs_display", ...].
        self.extra_args = extra_args or ["-nogui", "-nonet", "-hda_test"]
        self.proc: subprocess.Popen | None = None
        self._stdout_buf = ""

    # ------- lifecycle -------

    def start(self) -> "Harness":
        cmd = [self.rvvm, self.firmware, *self.extra_args]
        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,  # merge — easier to grep
            bufsize=0,
        )
        return self

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    def __enter__(self) -> "Harness":
        return self.start()

    def __exit__(self, *_exc) -> None:
        self.stop()

    # ------- low-level I/O -------

    def _read_some(self, timeout: float = 0.5) -> str:
        if self.proc is None or self.proc.stdout is None:
            return ""
        end = time.monotonic() + timeout
        chunks: list[str] = []
        while True:
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            r, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if not r:
                break
            data = os.read(self.proc.stdout.fileno(), 4096)
            if not data:
                break
            chunks.append(data.decode("utf-8", errors="replace"))
        return "".join(chunks)

    def read_until(self, pattern: str, timeout: float = 8.0) -> str:
        """Drain UART until `pattern` (regex) is seen. Returns the captured text."""
        end = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < end:
            self._stdout_buf += self._read_some(timeout=0.2)
            m = rx.search(self._stdout_buf)
            if m:
                consumed = self._stdout_buf[: m.end()]
                self._stdout_buf = self._stdout_buf[m.end():]
                return consumed
        # Timeout — return what we have for diagnostics.
        leftover = self._stdout_buf
        self._stdout_buf = ""
        raise TimeoutError(
            f"pattern {pattern!r} not seen within {timeout}s. "
            f"Last UART data:\n{leftover[-2000:]}"
        )

    def write(self, s: str) -> None:
        if self.proc is None or self.proc.stdin is None:
            return
        self.proc.stdin.write(s.encode("utf-8"))
        self.proc.stdin.flush()

    # ------- high-level helpers -------

    def wait_boot(self, timeout: float = 15.0) -> None:
        """Wait until BASIC has fully booted — drain UART, then poll the
        screen until "1982 Sinclair" shows up. Returns immediately when
        the © message is on screen."""
        self.read_until(r"Booting Sinclair BASIC", timeout=timeout)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            time.sleep(0.5)
            screen = self.screenshot()
            if "1982 Sinclair" in screen:
                return
        raise TimeoutError(
            f"BASIC didn't reach copyright print within {timeout}s"
        )

    def wait_settle(self, frames: float = 0.5) -> None:
        """Block while the firmware processes pending input."""
        time.sleep(frames)

    def type(self, s: str) -> None:
        """Send keystrokes via the UART. The firmware queues them."""
        for ch in s:
            self.write(ch)

    def command(self, cmd: str) -> str:
        """Run a debug command (`X[args]\\n`) and return the response text.
        Strips the local echo so the caller gets just the response body."""
        self._stdout_buf += self._read_some(timeout=0.05)  # drain stale
        self.write("`" + cmd + "\n")
        time.sleep(0.4)
        out = self._read_some(timeout=0.3)
        return out

    def drain_uart(self) -> str:
        """Return everything in the UART buffer right now — useful for
        capturing trace output that's interleaved with keystrokes."""
        return self._read_some(timeout=0.05)

    def screenshot(self) -> str:
        """Return the 24×32 ASCII visualisation of the screen."""
        out = self.command("v")
        # Pull out the framed ASCII grid: starts with a +-- line, ends
        # with a matching one. There may be other UART output before/
        # between (HID traces, etc.) — slice just the frame.
        m = re.search(
            r"^\+-{32}\+\n(?:\|[^\n]{32}\|\n){24}\+-{32}\+",
            out,
            re.MULTILINE,
        )
        return m.group(0) if m else out

    def memdump(self, addr: int) -> bytes:
        """Issue `m HEX` and return the 256 bytes parsed from the response."""
        out = self.command(f"m {addr:04x}")
        # Parse lines like: "  0x...c0: 0x3c 0x00 0x00 ... 0x00"
        # The firmware prints uart's %x = "0x..." prefix, so each byte
        # arrives as 0xNN.
        bytes_out = bytearray()
        for line in out.splitlines():
            m = re.match(r"\s*0x[0-9a-f]+:\s+(.*)", line)
            if not m:
                continue
            for tok in m.group(1).split():
                if tok.startswith("0x"):
                    try:
                        bytes_out.append(int(tok, 16) & 0xFF)
                    except ValueError:
                        pass
        return bytes(bytes_out[:256])

    def z80_state(self) -> dict[str, int]:
        """Issue `d` and parse out the Z80 register values."""
        out = self.command("d")
        regs: dict[str, int] = {}
        for k in ("pc", "sp", "af", "bc", "de", "hl",
                  "ix", "iy", "i", "r", "im", "iff1",
                  "border", "beeper", "frames", "pre_irq_pc"):
            m = re.search(rf"{k}=(0x)?([0-9a-fA-F]+)", out)
            if m:
                try:
                    regs[k] = int(m.group(2), 16)
                except ValueError:
                    pass
        return regs


# ------------- demo / smoke test entry point -------------

def _demo():
    """Spawn, boot, screenshot, and exit. Useful as a one-shot sanity check."""
    h = Harness().start()
    try:
        h.wait_boot()
        screen = h.screenshot()
        print("=== boot screenshot ===")
        print(screen)
        regs = h.z80_state()
        print(f"=== Z80 ===  pc={regs.get('pc'):x}  pre_irq_pc={regs.get('pre_irq_pc'):x}  frames={regs.get('frames')}")
    finally:
        h.stop()


if __name__ == "__main__":
    _demo()
