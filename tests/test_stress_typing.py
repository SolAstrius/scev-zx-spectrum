"""Stress-test typing many DIFFERENT letters in quick succession,
the way the user does in the bochs window. Each char is paced ~30 ms
apart — faster than human typing — to confirm the queue handles
real-world bursts."""

import time
from harness import Harness


def main():
    h = Harness().start()
    try:
        h.wait_boot()

        # First "PRINT" via K-mode 'p'.
        h.type("p")
        time.sleep(0.6)

        # Now type a bunch of different chars in L-mode. With the
        # same-key-gap optimisation, different keys can drain at full
        # rate (1 event/frame), so we should keep up.
        text = ' "hello world"'
        for ch in text:
            h.type(ch)
            time.sleep(0.03)
        time.sleep(2.0)

        # Don't run it (needs ENTER) — just snapshot the input line.
        screen = h.screenshot()
        for ln in screen.splitlines():
            if "PRINT" in ln:
                print(ln)
                if "hello world" in ln:
                    print("[ok] full string registered")
                else:
                    print("[FAIL] expected 'hello world' in line")
                break
    finally:
        h.stop()


if __name__ == "__main__":
    main()
