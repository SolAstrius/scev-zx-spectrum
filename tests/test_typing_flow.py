"""Show the screen evolving as we type. Should reproduce: © visible at
boot, vanishes once typing begins, BASIC processes commands."""

import time
from harness import Harness


def show(h, label):
    print(f"\n=== {label} ===")
    s = h.screenshot()
    # Print just the rows with non-blank content + first/last row to
    # save terminal real-estate.
    for line in s.splitlines():
        if line.startswith("|") and line.strip("|").strip():
            print(line)
        elif line.startswith("+"):
            print(line)


def main():
    h = Harness().start()
    try:
        h.wait_boot()
        show(h, "after boot — © visible at bottom")

        # P in K-mode should tokenise to "PRINT " and replace the © line.
        h.type("p")
        time.sleep(1.5)
        show(h, "after typing 'p' (K-mode → PRINT keyword)")

        # Add a space + a number + ENTER.
        h.type(" 42\r")
        time.sleep(1.5)
        show(h, "after 'p 42 ENTER' — BASIC should compute & print 42")
    finally:
        h.stop()


if __name__ == "__main__":
    main()
