"""Simulate the rapid-tap pattern from bochs HID — multiple DOWN/UP
of the same key in quick succession should each register as a
distinct keystroke, not get collapsed into one continuous press."""

import time
from harness import Harness


def main():
    h = Harness().start()
    try:
        h.wait_boot()

        # Type "9" five times — should appear as "99999" not "9".
        # We use a digit not a letter so K-mode tokenisation isn't
        # involved (every digit is itself in BASIC mode).
        # Actually — first switch out of K-mode by pressing ENTER on
        # an empty line so BASIC enters L-mode for next input.
        # Hmm — easier: type "PRINT" (one P), then digits.
        h.type("p")
        time.sleep(0.6)
        # Type each '9' with 100 ms between — that's faster than human
        # typing; if this works, the keyboard pipeline is solid.
        for _ in range(5):
            h.type("9")
            time.sleep(0.1)
        time.sleep(2.0)
        h.type("\r")
        time.sleep(2.0)
        screen = h.screenshot()
        print(screen)
        assert "99999" in screen, \
            f"expected '99999' (5 distinct digit presses), got:\n{screen}"
        print("[ok] all 5 fast 9-presses registered")
    finally:
        h.stop()


if __name__ == "__main__":
    main()
