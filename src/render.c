/* Speccy screen layout decoder.
 *
 * Bitmap @ 0x4000-0x57FF (6144 bytes, 256×192 mono), attributes @
 * 0x5800-0x5AFF (768 bytes, one per 8×8 cell). The bitmap address is
 * famously interleaved:
 *
 *   addr = 0x4000
 *        + (third       << 11)        // bits 12,11   y[7:6]
 *        + (line_in_char << 8)         // bits 10:8    y[2:0]
 *        + (char_row    << 5)          // bits 7:5     y[5:3]
 *        + x_byte                      // bits 4:0     x[7:3]
 *
 * Where: third = y / 64, char_row = (y % 64) / 8, line_in_char = y % 8.
 *
 * Attribute address is just `0x5800 + (y / 8) * 32 + x / 8`. Attribute
 * byte: bits 0..2 INK, 3..5 PAPER, 6 BRIGHT, 7 FLASH (we ignore
 * FLASH for now — it'd need a 32-frame counter to alternate ink/paper). */

#include "render.h"

/* Speccy palette in XRGB8888. Index = (bright << 3) | code.
 * Non-bright components are 0xCD (matches Fuse's default); bright
 * components are 0xFF. Black stays 0 in either case. */
static const uint32_t SPECCY_PALETTE[16] = {
    0x00000000, 0x000000CD, 0x00CD0000, 0x00CD00CD,
    0x0000CD00, 0x0000CDCD, 0x00CDCD00, 0x00CDCDCD,
    0x00000000, 0x000000FF, 0x00FF0000, 0x00FF00FF,
    0x0000FF00, 0x0000FFFF, 0x00FFFF00, 0x00FFFFFF,
};

static inline uint32_t bitmap_addr(uint32_t x, uint32_t y) {
    uint32_t third         = y >> 6;
    uint32_t line_in_third = y & 0x3F;
    uint32_t char_row      = line_in_third >> 3;
    uint32_t line_in_char  = line_in_third & 7;
    return SPECCY_SCREEN_BASE
         + (third       << 11)
         + (line_in_char << 8)
         + (char_row    << 5)
         + (x >> 3);
}

static inline uint32_t attr_addr(uint32_t x, uint32_t y) {
    return SPECCY_ATTR_BASE + ((y >> 3) * 32) + (x >> 3);
}

void speccy_render(speccy_t *vm, const gfx_t *g,
                   uint32_t x0, uint32_t y0, uint32_t scale) {
    if (scale == 0) scale = 1;
    uint32_t bw = SPECCY_BORDER * scale;
    uint32_t pw = SPECCY_SCREEN_W * scale;
    uint32_t ph = SPECCY_SCREEN_H * scale;
    uint32_t border_color = SPECCY_PALETTE[vm->border];

    /* Border — four rectangles around the picture area. Always
     * redrawn so a border-colour change between frames is visible
     * even when fb_dirty is false (the OUT $FE that changed it
     * doesn't touch screen RAM). */
    gfx_rect(g, x0,                y0,                pw + bw * 2, bw, border_color);            /* top */
    gfx_rect(g, x0,                y0 + bw + ph,      pw + bw * 2, bw, border_color);            /* bottom */
    gfx_rect(g, x0,                y0 + bw,           bw,          ph, border_color);            /* left */
    gfx_rect(g, x0 + bw + pw,      y0 + bw,           bw,          ph, border_color);            /* right */

    /* Picture area — only redo if something in screen-RAM changed.
     * Border-only updates skip this entirely. */
    if (!vm->fb_dirty) return;

    uint32_t pic_x0 = x0 + bw;
    uint32_t pic_y0 = y0 + bw;

    for (uint32_t y = 0; y < SPECCY_SCREEN_H; y++) {
        for (uint32_t cx = 0; cx < 32; cx++) {       /* 32 columns of 8 px */
            uint32_t x_pix = cx * 8;
            uint8_t  bits  = vm->mem[bitmap_addr(x_pix, y)];
            uint8_t  attr  = vm->mem[attr_addr(x_pix, y)];

            uint32_t ink   = SPECCY_PALETTE[(attr & 0x07) | ((attr & 0x40) >> 3)];
            uint32_t paper = SPECCY_PALETTE[((attr >> 3) & 0x07) | ((attr & 0x40) >> 3)];

            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t color = (bits & (0x80 >> bit)) ? ink : paper;
                uint32_t px = pic_x0 + (x_pix + bit) * scale;
                uint32_t py = pic_y0 + y * scale;
                /* Inline 1× / 2× to skip the per-pixel rect overhead
                 * for the common scales. Larger scales fall back to
                 * gfx_rect. */
                if (scale == 1) {
                    gfx_pixel(g, px, py, color);
                } else if (scale == 2) {
                    gfx_pixel(g, px,     py,     color);
                    gfx_pixel(g, px + 1, py,     color);
                    gfx_pixel(g, px,     py + 1, color);
                    gfx_pixel(g, px + 1, py + 1, color);
                } else {
                    gfx_rect(g, px, py, scale, scale, color);
                }
            }
        }
    }
    vm->fb_dirty = false;
}
