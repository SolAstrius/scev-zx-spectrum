#include "speccy.h"
#include <stddef.h>

/* External freestanding helpers from rvvm-hal/src/string.c. */
void *memset(void *dst, int c, unsigned long n);

/* Defined in keyboard.c. */
extern bool keyboard_translate(uint8_t usage, uint8_t *out_row, uint8_t *out_col);

/* Tweakable from debug.c — when true, speccy_step_frame skips the
 * Z80Interrupt call so we can observe the Z80 main loop without the
 * IRQ vector dominating PC samples. */
bool _debug_irq_disabled = false;

void speccy_reset(speccy_t *vm) {
    memset(vm->mem, 0, sizeof(vm->mem));
    /* All keys up — the matrix is active-low. Bits 5..7 stay set
     * because the ULA only owns bits 0..4. */
    for (int i = 0; i < 8; i++) vm->kbd[i] = 0xFF;
    for (int i = 0; i < 8 * 5; i++) vm->release_latch[i] = 0;
    vm->hid_head     = 0;
    vm->hid_tail     = 0;
    vm->gap_frames   = 0;
    vm->border       = 7;          /* white border on power-on */
    vm->beeper       = 0;
    vm->fb_dirty     = true;       /* force first render */
    vm->frame_count  = 0;
    vm->pre_irq_pc   = 0;
    Z80Reset(&vm->z80);
}

/* Hold each released key down for N frames so BASIC's IRQ-driven
 * keyboard scan sees it. The 48K ROM's KEYBOARD-INPUT uses a debounce
 * counter that starts at 5 and decrements every IRQ-frame the same
 * key is held; only when it hits 0 does the press get dispatched. */
#define KEY_RELEASE_LATCH  5

/* After a key is fully released (latch hits 0), wait this many
 * additional "all-up" frames before draining the next event from the
 * HID queue. BASIC's KSTATE retains the previous key in its
 * debounce/repeat tracking for several frames after release; if we
 * dispatch the next press too soon BASIC treats the press as part of
 * the previous keystroke's repeat sequence and refuses to dispatch
 * it as a new keystroke. */
#define KEY_RELEASE_GAP   3

/* HID queue helpers. Encoding: bit 7 = action (0=DOWN, 1=UP),
 * bits 6..3 = row (0..7), bits 2..0 = col (0..4). */
#define HID_QSIZE  ((int)sizeof(((speccy_t*)0)->hid_queue))

static inline uint8_t hid_pack(uint8_t row, uint8_t col, bool down) {
    return (uint8_t)((down ? 0 : 0x80) | ((row & 7) << 3) | (col & 7));
}
static inline void hid_unpack(uint8_t v, uint8_t *row, uint8_t *col, bool *down) {
    *down = (v & 0x80) == 0;
    *row  = (v >> 3) & 7;
    *col  = v & 7;
}

void speccy_kbd_enqueue(speccy_t *vm, uint8_t row, uint8_t col, bool pressed) {
    uint8_t next = (uint8_t)((vm->hid_tail + 1) % HID_QSIZE);
    if (next == vm->hid_head) return;   /* full — drop */
    vm->hid_queue[vm->hid_tail] = hid_pack(row, col, pressed);
    vm->hid_tail = next;
}

/* How many events are queued (used by debug.c to pace UART input
 * expansion against the speccy core's drain rate). */
uint8_t _speccy_hid_pending(speccy_t *vm) {
    return (uint8_t)((vm->hid_tail - vm->hid_head + HID_QSIZE) % HID_QSIZE);
}

/* Called once per frame from speccy_step_frame: tick release latches,
 * then drain at most one HID queue entry. The "one event per frame"
 * pacing is what makes repeated taps of the same key register as
 * distinct keystrokes — without it, an HID DOWN-UP-DOWN burst within
 * a single frame would just look like "still pressed" to BASIC. */
static void tick_release_latches(speccy_t *vm) {
    /* Snapshot whether any key was mid-release at the START of this
     * tick. If yes, we won't drain new events even after decrement —
     * we want BASIC's KEY-SCAN to see at least one full frame of
     * "all up" before the next press. Otherwise a DOWN drained in
     * the same tick where the previous release latch hit 0 would
     * make BASIC see continuous press across frames. */
    bool any_latched_before = false;
    for (int i = 0; i < 8 * 5; i++) {
        if (vm->release_latch[i] > 0) { any_latched_before = true; break; }
    }

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 5; col++) {
            uint8_t *latch = &vm->release_latch[row * 5 + col];
            if (*latch == 0) continue;
            if (--*latch == 0) {
                vm->kbd[row] |= (uint8_t)(1u << col);
            }
        }
    }

    /* If a release just completed in this tick, start a gap window:
     * BASIC's KSTATE keeps the previous key alive for several frames
     * after release. Dispatching the next press too quickly merges
     * into the existing entry. */
    if (any_latched_before) {
        bool any_latched_now = false;
        for (int i = 0; i < 8 * 5; i++) {
            if (vm->release_latch[i] > 0) { any_latched_now = true; break; }
        }
        if (!any_latched_now) vm->gap_frames = KEY_RELEASE_GAP;
        return;
    }

    if (vm->gap_frames > 0) { vm->gap_frames--; return; }
    if (vm->hid_head == vm->hid_tail) return;

    uint8_t row, col;
    bool    pressed;
    hid_unpack(vm->hid_queue[vm->hid_head], &row, &col, &pressed);
    vm->hid_head = (uint8_t)((vm->hid_head + 1) % HID_QSIZE);
    speccy_set_key(vm, row, col, pressed);
}

uint32_t speccy_step_frame(speccy_t *vm) {
    /* Decay any pending HID release latches before running the
     * frame, so newly-pressed keys are guaranteed to be observable
     * by the IRQ-time keyboard scan that runs inside Z80Emulate. */
    tick_release_latches(vm);

    /* Run a frame's worth of T-states, then raise the vblank IRQ.
     * z80emu's "interrupt" path injects the bus value (0xFF for IM 1
     * or as the low half of the IM 2 vector); the Speccy ULA puts
     * 0xFF on the bus and the ROM uses IM 1 by default, so 0xFF is
     * the correct payload either way. */
    uint32_t executed = (uint32_t)Z80Emulate(&vm->z80,
                                             SPECCY_T_PER_FRAME,
                                             vm);
    /* Snapshot PC for diagnostics — right after Z80Emulate, before
     * Z80Interrupt potentially clobbers it with 0x0038. */
    vm->pre_irq_pc = (uint16_t)vm->z80.pc;

    if (!_debug_irq_disabled) {
        Z80Interrupt(&vm->z80, 0xFF, vm);
    }
    vm->frame_count++;
    return executed;
}

uint8_t speccy_in(speccy_t *vm, uint16_t port) {
    /* ULA decode: any port with bit 0 = 0. The high byte selects
     * which keyboard half-rows to read (active-low: bit n = 0 →
     * scan row n). Multiple selected rows AND together — that's how
     * the ROM's "any key" scan works. */
    if ((port & 1) == 0) {
        uint8_t row_select = (uint8_t)(~(port >> 8));
        uint8_t result     = 0xFF;
        for (int row = 0; row < 8; row++) {
            if (row_select & (1u << row)) {
                result &= vm->kbd[row];
            }
        }
        /* Bits 5,7 always read 1; bit 6 = EAR (tape input) — tied
         * high since we have no tape. Mask to bits 0..4 of matrix. */
        return (uint8_t)((result & 0x1F) | 0xA0);
    }
    /* Unmapped ports: float to 0xFF on real hardware. */
    return 0xFF;
}

void speccy_out(speccy_t *vm, uint16_t port, uint8_t value) {
    if ((port & 1) == 0) {
        vm->border = (uint8_t)(value & 0x07);
        vm->beeper = (uint8_t)((value >> 4) & 1);
    }
}

bool speccy_hid_event(speccy_t *vm, uint8_t usage, bool pressed) {
    uint8_t row, col;
    if (!keyboard_translate(usage, &row, &col)) return false;
    /* Queue the event rather than applying it immediately. Drained
     * one-per-frame (when no key is mid-release) by tick_release_latches.
     * This paces fast HID bursts so each press → release transition is
     * visible to BASIC's KEYBOARD-SCAN. */
    speccy_kbd_enqueue(vm, row, col, pressed);
    return true;
}

void speccy_set_key(speccy_t *vm, uint8_t row, uint8_t col, bool pressed) {
    if (row >= 8 || col >= 5) return;
    if (pressed) {
        /* Force-release every key currently sitting in its release-
         * latch window. Bits with latch == 0 are either "all up" or
         * "actively held by host" — left alone. Bits with latch > 0
         * are stale leftovers from a recently-released key; if we
         * leave them BASIC's KEY-SCAN sees two keys held and treats
         * it as ambiguous (or, for shift+key, as a spurious shifted
         * variant of the new key). Dropping them here gives the new
         * press a clean matrix to land in.
         *
         * Active shift keys (held by the user across multiple typed
         * letters) are unaffected: their bits are cleared with
         * latch == 0, so the loop skips them. */
        for (int i = 0; i < 8 * 5; i++) {
            if (vm->release_latch[i] == 0) continue;
            int r = i / 5, c = i % 5;
            if (r == row && c == col) continue;
            vm->kbd[r] |= (uint8_t)(1u << c);
            vm->release_latch[i] = 0;
        }
        vm->kbd[row] &= (uint8_t)~(1u << col);
        vm->release_latch[row * 5 + col] = 0;
    } else {
        /* Defer the actual release: keep the bit cleared, set a latch
         * so it stays pressed for KEY_RELEASE_LATCH frames before
         * lifting. Real key-up is published when the latch decays. */
        vm->release_latch[row * 5 + col] = KEY_RELEASE_LATCH;
    }
}
