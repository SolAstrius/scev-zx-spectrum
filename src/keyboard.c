/* USB HID → ZX Spectrum 8×5 keyboard matrix.
 *
 * The Speccy's matrix half-rows, top-to-bottom (selected via inverted
 * bits in port 0xFE's high byte):
 *
 *   row  port_high  cols 0..4
 *   ---  ---------  ---------------------------
 *    0   0xFE       SHIFT  Z      X      C      V
 *    1   0xFD       A      S      D      F      G
 *    2   0xFB       Q      W      E      R      T
 *    3   0xF7       1      2      3      4      5
 *    4   0xEF       0      9      8      7      6
 *    5   0xDF       P      O      I      U      Y
 *    6   0xBF       ENTER  L      K      J      H
 *    7   0x7F       SPACE  SYM    M      N      B
 *
 * On a real Speccy, "CAPS SHIFT" (row 0 col 0) is the regular shift
 * key for SHIFT-printable mappings, and "SYMBOL SHIFT" (row 7 col 1)
 * is a separate key the host keyboard doesn't have a direct twin
 * for. We map host-Ctrl to SYMBOL SHIFT so :;,. and friends can be
 * typed via Ctrl+key on a modern keyboard. */

#include "keyboard.h"
#include <stddef.h>

/* `mapped` lets us distinguish a real CAPS SHIFT (row 0 col 0) from
 * a default-zero-initialised table slot. Unset slots have mapped=0. */
#define R(row, col)   { (row), (col), 1 }

typedef struct { uint8_t row, col, mapped; } key_t;

/* Indexed by USB HID usage code 0x00..0xE7. Sparse — most slots are
 * UNMAPPED. Sized to 0xE8 to cover modifier usage codes (LCTRL=0xE0
 * through RGUI=0xE7). */
static const key_t hid_to_speccy[0xE8] = {
    /* Letters: usage 0x04 = A through 0x1D = Z. */
    [0x04] = R(1, 0),  /* A */
    [0x05] = R(7, 4),  /* B */
    [0x06] = R(0, 3),  /* C */
    [0x07] = R(1, 2),  /* D */
    [0x08] = R(2, 2),  /* E */
    [0x09] = R(1, 3),  /* F */
    [0x0A] = R(1, 4),  /* G */
    [0x0B] = R(6, 4),  /* H */
    [0x0C] = R(5, 2),  /* I */
    [0x0D] = R(6, 3),  /* J */
    [0x0E] = R(6, 2),  /* K */
    [0x0F] = R(6, 1),  /* L */
    [0x10] = R(7, 2),  /* M */
    [0x11] = R(7, 3),  /* N */
    [0x12] = R(5, 1),  /* O */
    [0x13] = R(5, 0),  /* P */
    [0x14] = R(2, 0),  /* Q */
    [0x15] = R(2, 3),  /* R */
    [0x16] = R(1, 1),  /* S */
    [0x17] = R(2, 4),  /* T */
    [0x18] = R(5, 3),  /* U */
    [0x19] = R(0, 4),  /* V */
    [0x1A] = R(2, 1),  /* W */
    [0x1B] = R(0, 2),  /* X */
    [0x1C] = R(5, 4),  /* Y */
    [0x1D] = R(0, 1),  /* Z */

    /* Numbers (top row): 0x1E = 1 through 0x27 = 0. */
    [0x1E] = R(3, 0),  /* 1 */
    [0x1F] = R(3, 1),  /* 2 */
    [0x20] = R(3, 2),  /* 3 */
    [0x21] = R(3, 3),  /* 4 */
    [0x22] = R(3, 4),  /* 5 */
    [0x23] = R(4, 4),  /* 6 */
    [0x24] = R(4, 3),  /* 7 */
    [0x25] = R(4, 2),  /* 8 */
    [0x26] = R(4, 1),  /* 9 */
    [0x27] = R(4, 0),  /* 0 */

    /* Specials. */
    [0x28] = R(6, 0),  /* ENTER */
    [0x2C] = R(7, 0),  /* SPACE */
    [0x29] = R(7, 1),  /* ESC → SYMBOL SHIFT (so Speccy "EDIT" via SS+1 etc.
                          can be typed even from a host without Ctrl) */

    /* Backspace = CAPS+0 on Speccy. We can't trivially do a chord from
     * a single keystroke; map plain Backspace to "0" and let the user
     * hold Shift if they need DELETE. Improvement opportunity. */
    [0x2A] = R(4, 0),  /* BACKSPACE → 0 */

    /* Modifiers — usage codes 0xE0..0xE7. */
    [0xE0] = R(7, 1),  /* LCTRL  → SYMBOL SHIFT */
    [0xE1] = R(0, 0),  /* LSHIFT → CAPS SHIFT */
    [0xE2] = R(7, 1),  /* LALT   → SYMBOL SHIFT (alias) */
    [0xE4] = R(7, 1),  /* RCTRL  → SYMBOL SHIFT */
    [0xE5] = R(0, 0),  /* RSHIFT → CAPS SHIFT */
    [0xE6] = R(7, 1),  /* RALT   → SYMBOL SHIFT */
};

bool keyboard_translate(uint8_t usage, uint8_t *out_row, uint8_t *out_col) {
    if (usage >= sizeof(hid_to_speccy) / sizeof(hid_to_speccy[0])) return false;
    key_t k = hid_to_speccy[usage];
    if (!k.mapped) return false;
    *out_row = k.row;
    *out_col = k.col;
    return true;
}
