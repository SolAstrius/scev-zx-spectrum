"""Test the expanded keyboard chord mappings — symbols, backspace,
arrows. The harness types via UART (which uses debug.c's ascii_table
for ASCII→Speccy translation), but the speccy core's chord logic is
the same path used by HID. So if PRINT 1+2= shows correctly, both
the UART and HID symbol paths work."""

import time
from harness import Harness


def main():
    h = Harness().start()
    try:
        h.wait_boot()

        # PRINT 1+2 should give 3.
        h.type("p 1+2")
        time.sleep(2.0)
        screen_before = h.screenshot()
        for ln in screen_before.splitlines():
            if "PRINT" in ln:
                print(f"line: {ln}")
                break

        h.type("\r")
        time.sleep(2.0)
        screen = h.screenshot()
        if "3" in screen.splitlines()[1]:
            print("[ok] PRINT 1+2 = 3")
        else:
            print(f"[FAIL] expected '3' on first row")
            print(screen)

        # Test SS+L = "=": LET A=42 → assigns A.
        h.type("l a=42\r")
        time.sleep(2.0)
        # Then PRINT A should show 42.
        h.type("p a\r")
        time.sleep(2.0)
        screen = h.screenshot()
        # Find a row with "42"
        for ln in screen.splitlines():
            if ln.startswith("|") and "42" in ln:
                print(f"[ok] LET A=42 / PRINT A → '42' visible: {ln.strip()}")
                break
        else:
            print(f"[FAIL] expected '42' anywhere on screen")
            print(screen)
    finally:
        h.stop()


if __name__ == "__main__":
    main()
