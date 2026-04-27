#include "speccy.h"
#include <stddef.h>

/* External freestanding helpers from rvvm-hal/src/string.c. */
void *memset(void *dst, int c, unsigned long n);

/* Defined in keyboard.c. */
extern bool keyboard_translate(uint8_t usage, uint8_t *out_row, uint8_t *out_col);

void speccy_reset(speccy_t *vm) {
    memset(vm->mem, 0, sizeof(vm->mem));
    /* All keys up — the matrix is active-low. Bits 5..7 stay set
     * because the ULA only owns bits 0..4. */
    for (int i = 0; i < 8; i++) vm->kbd[i] = 0xFF;
    vm->border       = 7;          /* white border on power-on */
    vm->beeper       = 0;
    vm->fb_dirty     = true;       /* force first render */
    vm->frame_count  = 0;
    Z80Reset(&vm->z80);
}

uint32_t speccy_step_frame(speccy_t *vm) {
    /* Run a frame's worth of T-states, then raise the vblank IRQ.
     * z80emu's "interrupt" path injects the bus value (0xFF for IM 1
     * or as the low half of the IM 2 vector); the Speccy ULA puts
     * 0xFF on the bus and the ROM uses IM 1 by default, so 0xFF is
     * the correct payload either way. */
    uint32_t executed = (uint32_t)Z80Emulate(&vm->z80,
                                             SPECCY_T_PER_FRAME,
                                             vm);
    Z80Interrupt(&vm->z80, 0xFF, vm);
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
    speccy_set_key(vm, row, col, pressed);
    return true;
}

void speccy_set_key(speccy_t *vm, uint8_t row, uint8_t col, bool pressed) {
    if (row >= 8 || col >= 5) return;
    if (pressed) vm->kbd[row] &= (uint8_t)~(1u << col);
    else         vm->kbd[row] |= (uint8_t)(1u << col);
}
