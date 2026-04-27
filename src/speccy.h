/* ZX Spectrum 48K core state.
 *
 * Wraps z80emu (anotherlin) with the bits the original Speccy added on
 * top of a bare Z80: 64 KB unified address space (16 KB ROM @ 0x0000,
 * 48 KB RAM @ 0x4000), the ULA port (0xFE — keyboard read, border /
 * beeper / mic write), and a frame-paced step model with the 50 Hz
 * vblank IRQ. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "z80emu.h"

#define SPECCY_RAM_SIZE     0x10000          /* full Z80 address space */
#define SPECCY_ROM_SIZE     0x4000           /* 16 KB ROM at 0x0000 */
#define SPECCY_SCREEN_BASE  0x4000
#define SPECCY_SCREEN_PIXELS 0x1800          /* 6 KB bitmap */
#define SPECCY_ATTR_BASE    0x5800
#define SPECCY_ATTR_SIZE    0x300            /* 768 bytes attribute area */

#define SPECCY_SCREEN_W     256
#define SPECCY_SCREEN_H     192
#define SPECCY_BORDER       32               /* per-side */

/* 48K Speccy: 3.5 MHz Z80, 312 PAL scanlines × 224 T-states each =
 * 69,888 T-states/frame. Frame interrupt fires at the start of every
 * frame (held active for 32 T-states; we just pulse it once). */
#define SPECCY_T_PER_FRAME  69888
#define SPECCY_FPS          50

typedef struct {
    Z80_STATE z80;
    uint8_t   mem[SPECCY_RAM_SIZE];

    /* 8 keyboard half-rows × 5 columns. Bit n cleared = key in column
     * n is currently pressed. Bits 5..7 stay 1 (unused per ULA spec). */
    uint8_t   kbd[8];

    /* Per-key release latch: when speccy_set_key(... false) is called
     * we defer the actual bit-set in kbd[] for KEY_RELEASE_LATCH frames.
     * That ensures BASIC's IRQ-driven KEYBOARD-SCAN observes the key
     * pressed for at least a few frames even when the host's HID sends
     * a press and release within one emulator frame.
     *
     * Indexed [row*5 + col]; 0 = key currently up, >0 = release pending
     * in N frames. The bit in kbd[row] is held LOW (pressed) while
     * either the key is genuinely held OR the latch is non-zero. */
    uint8_t   release_latch[8 * 5];

    /* HID event queue. Real HID DOWN/UP pairs from bochs come in
     * faster than the Speccy's 50 Hz IRQ scan can resolve, so we
     * queue them and drain at most one event per emulator frame.
     * That gives BASIC's KEYBOARD-SCAN a clean transition for every
     * keystroke instead of intra-frame DOWN+UP getting collapsed.
     *
     * Encoding: each entry is (event_type << 7) | (row << 3) | col,
     * where event_type 0 = DOWN, 1 = UP. Capacity 64 is plenty for
     * sustained burst typing. */
    uint8_t   hid_queue[64];
    uint8_t   hid_head;
    uint8_t   hid_tail;
    uint8_t   gap_frames;   /* frames of all-up to wait before next same-key press */
    uint8_t   last_released_packed;  /* (row<<3)|col; 0xFF = none */

    /* Last write to port 0xFE: low 3 bits = border colour, bit 4 =
     * beeper level. Bit 3 (MIC) is ignored; bit 5 unused. */
    uint8_t   border;
    uint8_t   beeper;

    /* Cached "the screen-bytes part of mem changed since last render"
     * flag — saves a full 6912-byte memcmp per frame when nothing
     * drew. Set by Z80_WRITE_BYTE when the address falls in the
     * screen window. */
    bool      fb_dirty;

    uint64_t  frame_count;

    /* Snapshot of Z80 PC right BEFORE the per-frame Z80Interrupt call.
     * After step_frame returns, z80.pc is always 0x0038 (the IM 1
     * vector); this field records where the Z80 actually was at
     * frame-end. Useful to tell whether the main loop is making
     * progress or whether everything is stuck inside the IRQ handler. */
    uint16_t  pre_irq_pc;
} speccy_t;

/* Power-on reset: clears RAM, sets Z80 to known state, fills callbacks
 * (caller-supplied context = `vm`). Caller is responsible for loading
 * the ROM into mem[0..0x3FFF] separately. */
void speccy_reset(speccy_t *vm);

/* Run one PAL frame's worth of T-states, then assert IRQ once.
 * Returns the number of T-states actually executed (may differ from
 * SPECCY_T_PER_FRAME if the last instruction overshot the budget). */
uint32_t speccy_step_frame(speccy_t *vm);

/* USB HID usage code → keyboard matrix translation (handled in
 * keyboard.c). Convenience entry point so main.c doesn't import the
 * full mapping table. Enqueues the event for paced delivery to the
 * matrix. Returns true if the key was a known Speccy key, false if
 * it was unmapped (caller can ignore). */
bool speccy_hid_event(speccy_t *vm, uint8_t usage, bool pressed);

/* Direct matrix poke — used by snapshot loaders or test fixtures.
 * Bypasses the HID queue. */
void speccy_set_key(speccy_t *vm, uint8_t row, uint8_t col, bool pressed);

/* Enqueue an HID-style press/release for delivery on a future frame.
 * Used by speccy_hid_event() and any other "paced" input sources.
 * If the queue is full the event is dropped (and a warning logged). */
void speccy_kbd_enqueue(speccy_t *vm, uint8_t row, uint8_t col, bool pressed);

/* Implemented in speccy.c — exposed so z80user.h's macros can call
 * them without needing the full speccy_t internals visible to every
 * compilation unit. */
uint8_t speccy_in (speccy_t *vm, uint16_t port);
void    speccy_out(speccy_t *vm, uint16_t port, uint8_t value);
