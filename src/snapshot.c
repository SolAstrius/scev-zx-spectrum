/* .sna and .z80 (v1, v2, v3) snapshot loaders for 48K Speccy state.
 *
 * .sna (49179 bytes): 27-byte header + 48 KB RAM dump. PC is on the
 * stack at SP — we pop it back into z80.pc after copying RAM.
 *
 * .z80 v1: 30-byte header + (optionally compressed) 48 KB RAM. PC at
 * header bytes 6-7; if non-zero this is v1, if zero v2/v3 follows.
 * v1 RAM is compressed when bit 5 of header byte 12 is set; the RLE
 * is `0xED 0xED N V` for N copies of V, terminated with `00 ED ED 00`.
 *
 * .z80 v2/v3: v1 header with PC=0, then 23 (v2) or 54/55 (v3) bytes
 * of extended header, then per-page blocks. Each block: 2-byte byte
 * length (or 0xFFFF for uncompressed), 1-byte page number, data. For
 * 48K mode (HW byte = 0 or 1) only pages 4/5/8 are meaningful and
 * map to 0x8000 / 0xC000 / 0x4000 respectively.
 *
 * 128K mode (HW byte = 3, 4, ...) is rejected — needs paged RAM that
 * the 48K core doesn't model. */

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

/* .z80 RLE: a run of N copies of V is encoded as `ED ED N V`. v1 ends
 * with the terminator `00 ED ED 00` (a literal 00 followed by ED ED 00 00
 * which decodes as zero copies); v2/v3 don't use the terminator since
 * each page block carries an explicit byte length. We accept either.
 * Returns true if exactly `dstlen` bytes were produced. */
static bool decompress_z80(const uint8_t *src, uint32_t srclen,
                           uint8_t *dst, uint32_t dstlen) {
    uint32_t i = 0, o = 0;
    while (i < srclen && o < dstlen) {
        /* End-of-stream sentinel — output nothing further. */
        if (i + 3 < srclen && src[i] == 0xED && src[i+1] == 0xED
            && src[i+2] == 0 && src[i+3] == 0) {
            break;
        }
        if (i + 3 < srclen && src[i] == 0xED && src[i+1] == 0xED) {
            uint8_t n = src[i+2];
            uint8_t v = src[i+3];
            i += 4;
            while (n-- && o < dstlen) dst[o++] = v;
        } else {
            dst[o++] = src[i++];
        }
    }
    return o == dstlen;
}

/* Load 16 KiB into vm->mem[target..]. RVVM .z80 page blocks store
 * the data length explicitly; 0xFFFF means "16 KiB uncompressed". */
static bool load_z80_page(speccy_t *vm, uint16_t target,
                          const uint8_t *p, uint32_t blklen) {
    if (blklen == 0xFFFF) {
        memcpy(&vm->mem[target], p, 16384);
        return true;
    }
    return decompress_z80(p, blklen, &vm->mem[target], 16384);
}

static bool load_z80(speccy_t *vm, const uint8_t *buf, uint32_t len) {
    if (len < 30) return false;

    Z80_STATE *z = &vm->z80;
    Z80Reset(z);

    z->registers.byte[Z80_A] = buf[0];
    z->registers.byte[Z80_F] = buf[1];
    set_word(z, Z80_BC, (uint16_t)(buf[2]  | (buf[3]  << 8)));
    set_word(z, Z80_HL, (uint16_t)(buf[4]  | (buf[5]  << 8)));
    uint16_t pc_v1 = (uint16_t)(buf[6] | (buf[7] << 8));
    set_word(z, Z80_SP, (uint16_t)(buf[8] | (buf[9] << 8)));
    z->i = buf[10];

    /* Byte 12 is documented as 0xFF meaning 1 for compatibility with
     * Microsoft's BASIC, which writes that value in old saves. The
     * behaviour-relevant bits are bit 0 (R bit 7), bits 1-3 (border),
     * bit 5 (compression flag for v1). */
    uint8_t flags1 = buf[12];
    if (flags1 == 0xFF) flags1 = 1;

    z->r = (buf[11] & 0x7F) | ((flags1 & 1) ? 0x80 : 0);
    vm->border = (flags1 >> 1) & 0x07;
    bool compressed_v1 = (flags1 >> 5) & 1;

    set_word(z, Z80_DE, (uint16_t)(buf[13] | (buf[14] << 8)));
    z->alternates[Z80_BC] = (uint16_t)(buf[15] | (buf[16] << 8));
    z->alternates[Z80_DE] = (uint16_t)(buf[17] | (buf[18] << 8));
    z->alternates[Z80_HL] = (uint16_t)(buf[19] | (buf[20] << 8));
    /* Z80_AF stores A in the high byte. .z80 puts A' at byte 21, F' at
     * byte 22; assemble high|low manually rather than reading a 16-bit
     * little-endian word (which would swap them). */
    z->alternates[Z80_AF] = (uint16_t)((buf[21] << 8) | buf[22]);
    set_word(z, Z80_IY, (uint16_t)(buf[23] | (buf[24] << 8)));
    set_word(z, Z80_IX, (uint16_t)(buf[25] | (buf[26] << 8)));
    z->iff1 = buf[27] ? 1 : 0;
    z->iff2 = buf[28] ? 1 : 0;
    z->im   = buf[29] & 0x03;

    if (pc_v1 != 0) {
        /* v1: 49152 bytes of RAM (compressed or not) following the
         * fixed 30-byte header. PC was already read from byte 6. */
        const uint8_t *data = buf + 30;
        uint32_t rem = len - 30;
        if (compressed_v1) {
            if (!decompress_z80(data, rem, &vm->mem[0x4000], 49152)) {
                uart_puts("z80 v1: decompression underflow\n");
                return false;
            }
        } else {
            if (rem < 49152) return false;
            memcpy(&vm->mem[0x4000], data, 49152);
        }
        z->pc = pc_v1;
        vm->fb_dirty = true;
        uart_printf("z80 v1: PC=%x SP=%x IM=%u border=%u %s\n",
                    (uint64_t)z->pc, (uint64_t)z->registers.word[Z80_SP],
                    (uint64_t)z->im, (uint64_t)vm->border,
                    compressed_v1 ? "compressed" : "raw");
        return true;
    }

    /* v2/v3: byte 6-7 is zero, real PC and HW mode live in the
     * extended header that follows. */
    if (len < 32) return false;
    uint16_t ext_len = (uint16_t)(buf[30] | (buf[31] << 8));
    if (len < 32u + ext_len) return false;
    const uint8_t *ext = buf + 32;

    z->pc            = (uint16_t)(ext[0] | (ext[1] << 8));
    uint8_t hw_mode  = ext[2];

    /* Hardware modes per the .z80 spec — for 48K snapshots only mode
     * 0 or 1 (48K, optionally with Interface 1 ROM paged in) is
     * meaningful. 128K modes need paged RAM and a different page
     * mapping; reject loudly so the user knows. */
    if (hw_mode != 0 && hw_mode != 1) {
        uart_printf("z80 v%u: HW mode %u not 48K — refusing\n",
                    (uint64_t)(ext_len == 23 ? 2 : 3), (uint64_t)hw_mode);
        return false;
    }

    /* Page blocks follow the extended header. Each: 2-byte length, 1-byte
     * page number, then the data. In 48K mode pages map to:
     *   page 4  → 0x8000-0xBFFF
     *   page 5  → 0xC000-0xFFFF
     *   page 8  → 0x4000-0x7FFF  (always at 0x4000)
     * Other pages can appear in v3 saves (ROM, Multiface) — skip them. */
    uint32_t pos = 32 + ext_len;
    int pages_loaded = 0;
    while (pos + 3 <= len) {
        uint16_t blklen = (uint16_t)(buf[pos] | (buf[pos+1] << 8));
        uint8_t  pageno = buf[pos+2];
        pos += 3;

        uint32_t consume = (blklen == 0xFFFF) ? 16384 : blklen;
        if (pos + consume > len) return false;

        uint16_t target = 0xFFFF;
        if (pageno == 4) target = 0x8000;
        else if (pageno == 5) target = 0xC000;
        else if (pageno == 8) target = 0x4000;

        if (target != 0xFFFF) {
            if (!load_z80_page(vm, target, &buf[pos], blklen)) {
                uart_printf("z80: page %u (target=%x) decompress failed\n",
                            (uint64_t)pageno, (uint64_t)target);
                return false;
            }
            pages_loaded++;
        }
        pos += consume;
    }

    vm->fb_dirty = true;
    uart_printf("z80 v%u: PC=%x SP=%x IM=%u border=%u, %d/3 48K pages\n",
                (uint64_t)(ext_len == 23 ? 2 : 3),
                (uint64_t)z->pc, (uint64_t)z->registers.word[Z80_SP],
                (uint64_t)z->im, (uint64_t)vm->border, pages_loaded);
    return pages_loaded == 3;
}

bool snapshot_load(speccy_t *vm, const uint8_t *buf, uint32_t len) {
    if (len < 30) return false;
    /* .z80 v2/v3 marker: bytes 6-7 (the v1 PC field) are 0x0000, which
     * indicates the extended header follows. Non-zero in .sna almost
     * always (would be HL in .sna) and in .z80 v1 (real PC). */
    if (buf[6] == 0 && buf[7] == 0 && len >= 32) {
        return load_z80(vm, buf, len);
    }
    /* .sna: 27-byte header + 48 KiB RAM = 49179 bytes minimum. NVMe
     * reads round up to a 512-byte sector, so we may have a few
     * trailing bytes — load_sna ignores those. */
    if (len >= 49179) {
        return load_sna(vm, buf, len);
    }
    /* Smaller payload — must be a compressed .z80 v1. */
    return load_z80(vm, buf, len);
}
