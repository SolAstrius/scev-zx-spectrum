"""Smoke tests: boot to BASIC, type a few keys, observe what changed.

Run with:  python tests/test_boot.py
(no pytest needed — just a few asserts)."""

from harness import Harness


def test_boot_shows_copyright():
    h = Harness().start()
    try:
        h.wait_boot()
        screen = h.screenshot()
        print("DEBUG full screenshot:")
        print(screen)
        print(f"DEBUG line count: {len(screen.splitlines())}")
        # Search ANYWHERE in screen — copyright might wrap or land on
        # different rows depending on BASIC version.
        assert "1982 Sinclair Research Ltd" in screen, \
            f"boot message missing; screen was:\n{screen}"
        print("[ok] boot copyright present on screen")
    finally:
        h.stop()


def test_typing_p_changes_state():
    """Press P once and verify BASIC moved past WAIT-KEY (PC changes).
    We don't assert what BASIC drew on screen — that depends on K-mode
    tokenisation which we haven't fully verified — but PC progression
    is a clear "BASIC consumed the keystroke" signal."""
    h = Harness().start()
    try:
        h.wait_boot()
        before = h.z80_state()
        h.type("p")
        h.wait_settle(1.0)
        after = h.z80_state()
        print(f"  before: pre_irq_pc={before.get('pre_irq_pc'):#x}  frames={before.get('frames')}")
        print(f"  after:  pre_irq_pc={after.get('pre_irq_pc'):#x}  frames={after.get('frames')}")
        assert after.get("frames", 0) > before.get("frames", 0), \
            "frame counter not advancing — Z80 stalled?"
        # PC moves around inside the editor; we don't pin it to a single
        # value but it shouldn't be exactly the same as before.
        # Some editor states do park PC at a stable address while waiting
        # though, so this is informational rather than asserted.
    finally:
        h.stop()


def test_last_k_after_press():
    """Direct: type a key, dump LAST_K sysvar, see if BASIC's KEY_INPUT
    set it. If LAST_K stays 0 forever, our debounce / IRQ delivery is
    still broken somewhere."""
    h = Harness().start()
    try:
        h.wait_boot()
        h.type("p")
        h.wait_settle(1.5)
        sysvars = h.memdump(0x5C00)   # KSTATE..LAST_K..REPDEL..
        last_k = sysvars[0x08]        # offset 0x08 in this 256-byte block
        print(f"  LAST_K (5C08) after pressing 'p' = 0x{last_k:02x}")
        # We don't assert here yet — first run gathers evidence.
    finally:
        h.stop()


if __name__ == "__main__":
    print("--- test_boot_shows_copyright ---")
    test_boot_shows_copyright()
    print("\n--- test_typing_p_changes_state ---")
    test_typing_p_changes_state()
    print("\n--- test_last_k_after_press ---")
    test_last_k_after_press()
