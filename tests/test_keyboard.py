"""Investigate why BASIC isn't dispatching keystrokes despite our matrix
being updated correctly."""

import time
from harness import Harness


def show_kbd_state(h):
    """Dump the matrix + keyboard sysvars in one shot."""
    out = h.command("k")
    return out


def test_kbd_state_at_rest():
    h = Harness().start()
    try:
        h.wait_boot()
        print(show_kbd_state(h))
    finally:
        h.stop()


def test_press_p_observe_kstate_changes():
    """Press 'p' and observe KSTATE (0x5C00..0x5C07) over time.
    KSTATE is BASIC's debounce state; it should change when keys are
    detected, before LAST_K is set."""
    h = Harness().start()
    try:
        h.wait_boot()

        before = h.memdump(0x5C00)
        print("KSTATE before:", before[:8].hex())
        print(f"LAST_K before:  0x{before[8]:02x}")
        print(f"FLAGS before:   0x{before[0x3B]:02x}")

        h.type("p")
        for tick in range(8):
            time.sleep(0.2)
            mem = h.memdump(0x5C00)
            print(f"  +{tick*0.2:.1f}s  KSTATE={mem[:8].hex()}  LAST_K=0x{mem[8]:02x}  FLAGS=0x{mem[0x3B]:02x}  matrix_kbd5=?")
    finally:
        h.stop()


def test_force_a_press_via_direct_matrix():
    """Bypass debounce: hold 'A' (row 1 col 0) directly via debug command,
    let BASIC see it for many frames, see if LAST_K eventually becomes
    set. If it stays 0, our IRQ delivery / keyboard scan path is broken
    on the firmware side (not just timing)."""
    h = Harness().start()
    try:
        h.wait_boot()
        # We don't have a "force kbd[N] = X" debug command yet — easiest
        # is to just send the char and rely on the latch.
        h.type("a")
        # Wait LONG: 5 seconds = 250 frames. Plenty for BASIC to do its
        # thing even with lazy scan.
        time.sleep(5.0)
        mem = h.memdump(0x5C00)
        print(f"after 5s of 'a' input:  LAST_K=0x{mem[8]:02x}  FLAGS=0x{mem[0x3B]:02x}")
        print(f"KSTATE: {mem[:8].hex()}")
        screen = h.screenshot()
        print("\nscreen after:")
        print(screen)
    finally:
        h.stop()


if __name__ == "__main__":
    print("--- kbd state at rest ---")
    test_kbd_state_at_rest()
    print("\n--- press p, observe ---")
    test_press_p_observe_kstate_changes()
    print("\n--- force A for 5s ---")
    test_force_a_press_via_direct_matrix()
