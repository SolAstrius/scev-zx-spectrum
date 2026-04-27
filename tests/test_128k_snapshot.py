"""Load a 128K .z80 v2 snapshot and verify the Z80 makes progress.

Uses Manic Miner-2000 (a community 128K hack) from the WoS 2017
mirror. The snapshot's PC is mid-game-loop (~0x92F8), not the 128K
menu — so success looks like "PC keeps moving" rather than "menu
appeared". """

import time
from pathlib import Path
from harness import Harness, pad_to_lba


def main():
    rom_128 = Path(__file__).resolve().parent.parent / "roms" / "128.rom"
    snap    = Path(__file__).resolve().parent.parent / "snapshots" / "manic_miner_2000.z80"
    if not rom_128.exists():
        raise SystemExit(f"missing {rom_128}")
    if not snap.exists():
        raise SystemExit(f"missing {snap}")

    snap_padded = pad_to_lba(snap)
    h = Harness(extra_args=[
        "-nogui", "-nonet", "-hda_test",
        "-nvme", str(rom_128),
        "-nvme", str(snap_padded),
    ]).start()
    try:
        h.read_until(r"snapshot loaded", timeout=10)
        time.sleep(2.0)
        regs1 = h.z80_state()
        time.sleep(2.0)
        regs2 = h.z80_state()

        f1 = regs1.get("frames", 0)
        f2 = regs2.get("frames", 0)
        pc1 = regs1.get("pre_irq_pc", 0)
        pc2 = regs2.get("pre_irq_pc", 0)
        print(f"t=2s: pre_irq_pc=0x{pc1:x}  frames={f1}")
        print(f"t=4s: pre_irq_pc=0x{pc2:x}  frames={f2}")
        if f2 - f1 < 50:
            print(f"FAIL: only {f2-f1} frames advanced in 2s — Z80 stuck")
            return 1
        attrs = h.memdump(0x5800)
        nz = sum(1 for b in attrs if b != 0)
        print(f"attr ram[0x5800..]: {nz}/{len(attrs)} non-zero  →  game screen drawn")
        print("PASS: 128K snapshot loaded and Z80 advancing")
        return 0
    finally:
        h.stop()


if __name__ == "__main__":
    raise SystemExit(main() or 0)
