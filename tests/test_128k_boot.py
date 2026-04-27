"""Boot 128K mode and screenshot the menu.

The ASCII renderer in debug.c matches glyphs against the 48K BASIC
font at rom[0x3D00] — but on 128K we boot with rom_idx=0 (editor ROM)
which has its menu in larger custom glyphs that don't match the 8×8
font. So the screenshot shows '?' for unrecognised cells. The
underlying screen bytes are fine — the bochs framebuffer renders the
menu correctly. Look at it visually with `make run-snap SNAP=…` or
RVVM directly with `-bochs_display -nvme roms/128.rom`."""

import time
from pathlib import Path
from harness import Harness


def main():
    rom_128 = Path(__file__).resolve().parent.parent / "roms" / "128.rom"
    if not rom_128.exists():
        raise SystemExit(f"missing {rom_128} — concat 128-0.rom + 128-1.rom first")
    h = Harness(extra_args=[
        "-nogui", "-nonet", "-hda_test", "-nvme", str(rom_128),
    ]).start()
    try:
        h.read_until(r"ROM: 128K", timeout=10)
        time.sleep(5.0)

        print("=== 128K menu (5s after boot) ===")
        print(h.screenshot())

        regs = h.z80_state()
        print(f"PC=0x{regs.get('pc', 0):x} (IM1 vector if 0x38)  "
              f"pre_irq_pc=0x{regs.get('pre_irq_pc', 0):x}  "
              f"frames={regs.get('frames')}")

        # Spot check: read attribute RAM (bank 5 @ 0x5800). 256 bytes
        # come back from memdump. Non-zero count proves the editor
        # really laid down a menu screen — all zero would mean we're
        # stuck in the startup loop.
        attrs = h.memdump(0x5800)
        nonzero = sum(1 for b in attrs if b != 0)
        print(f"attr ram[0x5800..]: {nonzero}/{len(attrs)} non-zero")
    finally:
        h.stop()


if __name__ == "__main__":
    main()
