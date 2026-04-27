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
 * full mapping table. Returns true if the key was a known Speccy
 * key, false if it was unmapped (caller can ignore). */
bool speccy_hid_event(speccy_t *vm, uint8_t usage, bool pressed);

/* Direct matrix poke — used by snapshot loaders or test fixtures. */
void speccy_set_key(speccy_t *vm, uint8_t row, uint8_t col, bool pressed);

/* Implemented in speccy.c — exposed so z80user.h's macros can call
 * them without needing the full speccy_t internals visible to every
 * compilation unit. */
uint8_t speccy_in (speccy_t *vm, uint16_t port);
void    speccy_out(speccy_t *vm, uint16_t port, uint8_t value);
