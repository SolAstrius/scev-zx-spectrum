"""Simplest possible repeated-key test: type p, then check the
input buffer (E_LINE area) after each '9' to see what's actually
landing."""

import time
from harness import Harness


def main():
    h = Harness().start()
    try:
        h.wait_boot()
        h.type("p")
        time.sleep(0.6)
        screen = h.screenshot()
        print("after 'p':")
        print(screen[-300:])

        for i in range(5):
            h.type("9")
            time.sleep(0.6)
            screen = h.screenshot()
            print(f"after '9' #{i+1}:")
            print(screen[-300:])
    finally:
        h.stop()


if __name__ == "__main__":
    main()
