/* ZX Spectrum 48K / 128K core state.
 *
 * Wraps z80emu (anotherlin) with the bits the original Speccy added on
 * top of a bare Z80: address-space banking, the ULA port (0xFE —
 * keyboard read, border / beeper / mic write), the 128K paging port
 * (0x7FFD — RAM bank, screen, ROM select, paging lock), and a
 * frame-paced step model with the 50 Hz vblank IRQ.
 *
 * Memory layout (Z80's 64 KiB view):
 *   0x0000-0x3FFF  ROM bank — 0 (128K editor) or 1 (48K BASIC) on 128K;
 *                  always 0 (= 48K BASIC) on 48K.
 *   0x4000-0x7FFF  RAM bank 5 (the standard screen).
 *   0x8000-0xBFFF  RAM bank 2.
 *   0xC000-0xFFFF  RAM bank N — selectable via port 0x7FFD on 128K;
 *                  pinned to bank 0 on 48K.
 *
 * 48K mode is just 128K with paging locked at boot. Same code paths,
 * same struct, just `mode_128k = false` and any 0x7FFD writes ignored. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "z80emu.h"

#define SPECCY_PAGE_SIZE     0x4000          /* 16 KiB per bank */
#define SPECCY_NUM_RAM_BANKS 8                /* 128 KiB total */
#define SPECCY_NUM_ROM_BANKS 2                /* 32 KiB total */

#define SPECCY_SCREEN_BASE   0x4000           /* RAM bank 5 mapped here */
#define SPECCY_SCREEN_PIXELS 0x1800           /* 6 KB bitmap */
#define SPECCY_ATTR_BASE     0x5800
#define SPECCY_ATTR_SIZE     0x300            /* 768 bytes attribute area */

#define SPECCY_SCREEN_W     256
#define SPECCY_SCREEN_H     192
#define SPECCY_BORDER       32                /* per-side */

/* T-states per frame. 48K: 312 PAL scanlines × 224 T-states.
 * 128K: 311 scanlines × 228 T-states (slightly different ULA timing).
 * Speccy_t::t_per_frame is set at boot from mode_128k. */
#define SPECCY_T_PER_FRAME_48K   69888
#define SPECCY_T_PER_FRAME_128K  70908
#define SPECCY_FPS               50

/* Port 0x7FFD bits (128K paging). */
#define SPECCY_7FFD_RAM_MASK    0x07         /* bits 0..2 → bank at 0xC000 */
#define SPECCY_7FFD_SCREEN      0x08         /* 0 = bank 5, 1 = bank 7 */
#define SPECCY_7FFD_ROM         0x10         /* 0 = ROM 0 (128K), 1 = ROM 1 (48K) */
#define SPECCY_7FFD_LOCK        0x20         /* once set, port becomes RO until reset */

typedef struct {
    Z80_STATE z80;

    /* Bank-resident memory. The Z80 sees these via page_*; writes go
     * directly into the right bank without copying. */
    uint8_t   rom[SPECCY_NUM_ROM_BANKS][SPECCY_PAGE_SIZE];
    uint8_t   ram[SPECCY_NUM_RAM_BANKS][SPECCY_PAGE_SIZE];

    /* Live page mapping. page_0000 changes on ROM-select; page_C000
     * changes on RAM-bank select. page_4000 / page_8000 are pinned to
     * banks 5 and 2 respectively (Spectrum hardware never moves them). */
    const uint8_t *page_0000;
          uint8_t *page_4000;
          uint8_t *page_8000;
          uint8_t *page_C000;

    /* Paging state — last value written to port 0x7FFD before any
     * lock, plus expanded fields. Kept separate from the live page_*
     * pointers so a snapshot loader can stamp them and then call
     * speccy_apply_paging() to refresh the pointers. */
    uint8_t   port_7ffd;
    uint8_t   ram_idx;          /* 0..7: bank at 0xC000 */
    uint8_t   rom_idx;          /* 0..1: bank at 0x0000 */
    uint8_t   screen_idx;       /* 5 (normal) or 7 (shadow) — render-side hint */
    bool      paging_locked;
    bool      mode_128k;
    uint32_t  t_per_frame;

    /* 8 keyboard half-rows × 5 columns. Bit n cleared = key in column
     * n is currently pressed. Bits 5..7 stay 1 (unused per ULA spec). */
    uint8_t   kbd[8];

    /* Per-key release latch (see speccy.c for rationale). */
    uint8_t   release_latch[8 * 5];

    /* HID event queue (see speccy.c). */
    uint8_t   hid_queue[64];
    uint8_t   hid_head;
    uint8_t   hid_tail;
    uint8_t   gap_frames;
    uint8_t   last_released_packed;

    /* Last write to port 0xFE: low 3 bits = border colour, bit 4 =
     * beeper level. Bit 3 (MIC) is ignored; bit 5 unused. */
    uint8_t   border;
    uint8_t   beeper;

    /* Cached "the screen-bytes part of mem changed since last render"
     * flag — saves a full memcmp per frame when nothing drew. */
    bool      fb_dirty;

    uint64_t  frame_count;

    /* PC right BEFORE the per-frame Z80Interrupt call. Useful for
     * debugging — distinguishes "main loop progressing" from "stuck
     * inside the IRQ handler". */
    uint16_t  pre_irq_pc;
} speccy_t;

/* Power-on reset. Caller is responsible for filling rom[0] (and rom[1]
 * if 128K mode) BEFORE calling reset, OR right after — both work since
 * Z80Reset just sets PC=0 and the first fetch from rom[0] won't happen
 * until step_frame runs. Mode is determined by `is_128k`:
 *   false → ROM 1 mirrored from ROM 0 ignored, paging locked from boot,
 *           t_per_frame = 48K.
 *   true  → both ROMs in use, paging unlocked, ROM 0 selected (= 128K
 *           menu screen on first boot), t_per_frame = 128K. */
void speccy_reset(speccy_t *vm, bool is_128k);

/* Run one PAL frame's worth of T-states, then assert IRQ once.
 * Returns the number of T-states actually executed. */
uint32_t speccy_step_frame(speccy_t *vm);

/* USB HID usage code → keyboard matrix translation (handled in
 * keyboard.c). */
bool speccy_hid_event(speccy_t *vm, uint8_t usage, bool pressed);

/* Direct matrix poke — bypasses the HID queue. */
void speccy_set_key(speccy_t *vm, uint8_t row, uint8_t col, bool pressed);

/* Enqueue an HID-style press/release for delivery on a future frame. */
void speccy_kbd_enqueue(speccy_t *vm, uint8_t row, uint8_t col, bool pressed);

/* Refresh page_* pointers from rom_idx/ram_idx/screen_idx. Called
 * automatically on port 0x7FFD writes and after snapshot loads. */
void speccy_apply_paging(speccy_t *vm);

/* I/O — implemented in speccy.c, exposed for z80user.h's macros. */
uint8_t speccy_in (speccy_t *vm, uint16_t port);
void    speccy_out(speccy_t *vm, uint16_t port, uint8_t value);

/* Bank-aware memory accessors. Inlined for the Z80 hot loop —
 * z80user.h's READ_BYTE / WRITE_BYTE macros expand to these. */
static inline uint8_t speccy_read8(const speccy_t *vm, uint16_t addr) {
    if (addr < 0x4000)       return vm->page_0000[addr];
    else if (addr < 0x8000)  return vm->page_4000[addr - 0x4000];
    else if (addr < 0xC000)  return vm->page_8000[addr - 0x8000];
    else                     return vm->page_C000[addr - 0xC000];
}

static inline void speccy_write8(speccy_t *vm, uint16_t addr, uint8_t v) {
    if (addr < 0x4000) return;                          /* ROM, ignore */
    if (addr < 0x8000) {
        vm->page_4000[addr - 0x4000] = v;
        if (addr < SPECCY_ATTR_BASE + SPECCY_ATTR_SIZE) vm->fb_dirty = true;
    } else if (addr < 0xC000) {
        vm->page_8000[addr - 0x8000] = v;
    } else {
        vm->page_C000[addr - 0xC000] = v;
        /* Shadow-screen writes via bank 7 also dirty the framebuffer
         * if 128K mode and shadow is the active display. */
        if (vm->screen_idx == 7 && vm->ram_idx == 7) vm->fb_dirty = true;
    }
}
