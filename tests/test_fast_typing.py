"""Stress-test the keyboard pipeline with fast typing.
Without the force-release-on-new-press fix, characters get dropped
because multiple keys end up "held" in the matrix simultaneously and
BASIC's KEY-SCAN treats it as ambiguous."""

import time
from harness import Harness


def main():
    h = Harness().start()
    try:
        h.wait_boot()

        # Type a multi-character BASIC command. Each char is queued
        # and dispatched with our debounce timing. If our force-release
        # logic works, every keystroke registers.
        # PRINT 1+2+3+4 → 10
        # In K-mode: P → "PRINT" token, then space, "1+2+3+4", ENTER.
        h.type("p 1+2+3+4\r")

        # Wait for BASIC to execute. Be generous.
        time.sleep(3.0)

        screen = h.screenshot()
        print(screen)

        if "10" in screen:
            print("[ok] BASIC computed 1+2+3+4 = 10")
        else:
            print("[FAIL] expected '10' on screen")

        if "0 OK" in screen:
            print("[ok] command exited with 0 OK")
        else:
            print("[FAIL] no '0 OK' report")
    finally:
        h.stop()


if __name__ == "__main__":
    main()
