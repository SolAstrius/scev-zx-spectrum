/* .sna and .z80 v1 snapshot loaders.
 *
 * .sna (49179 bytes): 27-byte header + 48 KB RAM dump. PC is on the
 * stack at SP — we pop it back into z80.pc after copying RAM.
 *
 * .z80 v1: 30-byte header + (optionally compressed) 48 KB RAM. PC at
 * header bytes 6-7; if non-zero this is v1, if zero it's a v2/v3
 * snapshot (extended header follows). v1 RAM is compressed when bit
 * 5 of header byte 12 is set, with a simple RLE: a run of N bytes of
 * value V is written as `0xED 0xED N V`, terminated by `0x00 0xED
 * 0xED 0x00`. Uncompressed v1 just blits 49152 bytes verbatim. */

#include "snapshot.h"
#include "speccy.h"
#include "uart.h"
#include <stddef.h>

void *memcpy(void *dst, const void *src, unsigned long n);

/* z80emu's register file is a union — 14 bytes (registers.byte[],
 * indexed by Z80_B/Z80_C/etc) overlaid on 7 words (registers.word[],
 * indexed by Z80_BC/Z80_DE/Z80_HL/Z80_AF/Z80_IX/Z80_IY/Z80_SP). The
 * Z80_*_pair constants share the same enumeration order, so writing
 * a pair via .word[Z80_BC] is the cleanest route. */
static inline void set_word(Z80_STATE *z, int pair_idx, uint16_t v) {
    z->registers.word[pair_idx] = v;
}

static bool load_sna(speccy_t *vm, const uint8_t *buf, uint32_t len) {
    if (len < 27 + 49152) return false;

    /* Header layout (Sinclair docs):
     *   0       I
     *   1..2    HL'
     *   3..4    DE'
     *   5..6    BC'
     *   7..8    AF'
     *   9..10   HL
     *   11..12  DE
     *   13..14  BC
     *   15..16  IY
     *   17..18  IX
     *   19      IFF (bit 2 = IFF2; both IFF1 and IFF2 share state)
     *   20      R
     *   21..22  AF
     *   23..24  SP
     *   25      IM (interrupt mode 0/1/2)
     *   26      border colour
     */
    Z80_STATE *z = &vm->z80;
    Z80Reset(z);

    z->i = buf[0];

    set_word(z, Z80_HL, (uint16_t)(buf[9]  | (buf[10] << 8)));
    set_word(z, Z80_DE, (uint16_t)(buf[11] | (buf[12] << 8)));
    set_word(z, Z80_BC, (uint16_t)(buf[13] | (buf[14] << 8)));
    set_word(z, Z80_IY, (uint16_t)(buf[15] | (buf[16] << 8)));
    set_word(z, Z80_IX, (uint16_t)(buf[17] | (buf[18] << 8)));

    z->alternates[Z80_HL] = (uint16_t)(buf[1] | (buf[2] << 8));
    z->alternates[Z80_DE] = (uint16_t)(buf[3] | (buf[4] << 8));
    z->alternates[Z80_BC] = (uint16_t)(buf[5] | (buf[6] << 8));
    z->alternates[Z80_AF] = (uint16_t)(buf[7] | (buf[8] << 8));

    z->iff1 = z->iff2 = (buf[19] >> 2) & 1;
    z->r    = buf[20];
    set_word(z, Z80_AF, (uint16_t)(buf[21] | (buf[22] << 8)));
    set_word(z, Z80_SP, (uint16_t)(buf[23] | (buf[24] << 8)));
    z->im   = buf[25] & 0x03;

    /* RAM dump goes at 0x4000-0xFFFF. */
    memcpy(&vm->mem[0x4000], &buf[27], 49152);

    /* PC is on the stack — pop it. The .sna format stores the
     * snapshot's state as if it had just been interrupted, so PC
     * was pushed by the IRQ. */
    uint16_t sp = z->registers.word[Z80_SP];
    uint16_t pc = (uint16_t)(vm->mem[sp] | (vm->mem[(uint16_t)(sp + 1)] << 8));
    z->registers.word[Z80_SP] = (uint16_t)(sp + 2);
    z->pc = pc;

    vm->border   = (uint8_t)(buf[26] & 0x07);
    vm->fb_dirty = true;
    uart_printf("sna: PC=%x SP=%x I=%x IM=%u border=%u\n",
                (uint64_t)pc, (uint64_t)z->registers.word[Z80_SP],
                (uint64_t)z->i, (uint64_t)z->im, (uint64_t)vm->border);
    return true;
}

/* TODO: .z80 v1/v2/v3 — for v1 firmware just .sna. */

bool snapshot_load(speccy_t *vm, const uint8_t *buf, uint32_t len) {
    /* .sna is exactly 49,179 bytes for a 48K snapshot. We may have
     * read more than that off the disk (rounded up to sector); only
     * the size matters here. */
    if (len >= 49179) {
        return load_sna(vm, buf, len);
    }
    return false;
}
