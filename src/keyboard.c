/* USB HID → ZX Spectrum keyboard chord lookup.
 *
 * Each entry has two mappings: one for the key alone, one for the
 * key with host SHIFT held. For letters the shifted entry is just
 * the same cell + CAPS SHIFT (capital). For digits the shifted entry
 * is the same cell + SYMBOL SHIFT (giving "!@#$%..." instead of the
 * Speccy's CS+digit edit codes). For symbol keys the two entries
 * map to entirely different Speccy chords (e.g. host `=` → "="
 * unshifted (SS+L), `+` shifted (SS+K)). */

#include "keyboard.h"
#include <stddef.h>

/* Per-entry record: two chord mappings (plain + shifted) plus a
 * `defined` flag. Default-init = undefined; explicit entries below. */
typedef struct {
    kbd_chord_t plain;
    kbd_chord_t shifted;
    uint8_t     defined;
} key_def_t;

#define UNDEF       { {0, 0, 0}, {0, 0, 0}, 0 }
#define K(r, c)     {{(r), (c), KBD_NO_SHIFT     }, {(r), (c), KBD_CAPS_SHIFT  }, 1}
#define DIGIT(r, c) {{(r), (c), KBD_NO_SHIFT     }, {(r), (c), KBD_SYMBOL_SHIFT}, 1}
#define SYM(r, c, sr, sc) \
                    {{(r), (c), KBD_SYMBOL_SHIFT }, {(sr), (sc), KBD_SYMBOL_SHIFT}, 1}
#define CS(r, c)    {{(r), (c), KBD_CAPS_SHIFT   }, {(r), (c), KBD_CAPS_SHIFT  }, 1}
#define SS(r, c)    {{(r), (c), KBD_SYMBOL_SHIFT }, {(r), (c), KBD_SYMBOL_SHIFT}, 1}
#define PLAIN(r, c) {{(r), (c), KBD_NO_SHIFT     }, {(r), (c), KBD_NO_SHIFT    }, 1}

static const key_def_t kbd_table[0xE8] = {
    /* Letters: usage 0x04..0x1D = a..z. Plain = lowercase, shifted = CS+letter. */
    [0x04] = K(1, 0),  /* A */  [0x05] = K(7, 4),  /* B */
    [0x06] = K(0, 3),  /* C */  [0x07] = K(1, 2),  /* D */
    [0x08] = K(2, 2),  /* E */  [0x09] = K(1, 3),  /* F */
    [0x0A] = K(1, 4),  /* G */  [0x0B] = K(6, 4),  /* H */
    [0x0C] = K(5, 2),  /* I */  [0x0D] = K(6, 3),  /* J */
    [0x0E] = K(6, 2),  /* K */  [0x0F] = K(6, 1),  /* L */
    [0x10] = K(7, 2),  /* M */  [0x11] = K(7, 3),  /* N */
    [0x12] = K(5, 1),  /* O */  [0x13] = K(5, 0),  /* P */
    [0x14] = K(2, 0),  /* Q */  [0x15] = K(2, 3),  /* R */
    [0x16] = K(1, 1),  /* S */  [0x17] = K(2, 4),  /* T */
    [0x18] = K(5, 3),  /* U */  [0x19] = K(0, 4),  /* V */
    [0x1A] = K(2, 1),  /* W */  [0x1B] = K(0, 2),  /* X */
    [0x1C] = K(5, 4),  /* Y */  [0x1D] = K(0, 1),  /* Z */

    /* Digits: usage 0x1E..0x27 = 1..0. Plain = digit, shifted = SS+digit
     * (gives !@#$%^&*()_  on the host's printed legend). */
    [0x1E] = DIGIT(3, 0),  /* 1  / SHIFT+1 → ! (SS+1) */
    [0x1F] = DIGIT(3, 1),  /* 2  / @       (SS+2) */
    [0x20] = DIGIT(3, 2),  /* 3  / #       (SS+3) */
    [0x21] = DIGIT(3, 3),  /* 4  / $       (SS+4) */
    [0x22] = DIGIT(3, 4),  /* 5  / %       (SS+5) */
    [0x23] = DIGIT(4, 4),  /* 6  / &       (SS+6) */
    [0x24] = DIGIT(4, 3),  /* 7  / '       (SS+7) */
    [0x25] = DIGIT(4, 2),  /* 8  / (       (SS+8) */
    [0x26] = DIGIT(4, 1),  /* 9  / )       (SS+9) */
    [0x27] = DIGIT(4, 0),  /* 0  / _       (SS+0) — but host SHIFT+0 is normally ")"; we map it as documented */

    /* ENTER (no shift change). */
    [0x28] = PLAIN(6, 0),

    /* ESC → BREAK = CS+SPACE. */
    [0x29] = { {7, 0, KBD_CAPS_SHIFT}, {7, 0, KBD_CAPS_SHIFT}, 1 },

    /* BACKSPACE → DELETE = CS+0. */
    [0x2A] = { {4, 0, KBD_CAPS_SHIFT}, {4, 0, KBD_CAPS_SHIFT}, 1 },

    /* TAB — Speccy has no real TAB; map to SPACE for now. */
    [0x2B] = PLAIN(7, 0),

    /* SPACE. */
    [0x2C] = PLAIN(7, 0),

    /* US-layout symbol keys — plain produces the unshifted glyph,
     * "shifted" entry produces the host's printed shifted glyph
     * (translated to the appropriate Speccy chord).
     *
     *   key     plain   shifted
     *   ---     -----   -------
     *   - / _   SS+J    SS+0 (host SHIFT+- = _)
     *   = / +   SS+L    SS+K
     *   ; / :   SS+O    SS+Z
     *   ' / "   SS+7    SS+P
     *   , / <   SS+N    SS+R
     *   . / >   SS+M    SS+T
     *   / / ?   SS+V    SS+C
     */
    [0x2D] = SYM(6, 3, 4, 0),  /* - / _   = SS+J / SS+0 */
    [0x2E] = SYM(6, 1, 6, 2),  /* = / +   = SS+L / SS+K */
    [0x33] = SYM(5, 1, 0, 1),  /* ; / :   = SS+O / SS+Z */
    [0x34] = SYM(4, 3, 5, 0),  /* ' / "   = SS+7 / SS+P */
    [0x36] = SYM(7, 3, 2, 3),  /* , / <   = SS+N / SS+R */
    [0x37] = SYM(7, 2, 2, 4),  /* . / >   = SS+M / SS+T */
    [0x38] = SYM(0, 4, 0, 3),  /* / / ?   = SS+V / SS+C */

    /* Arrow keys → CAPS SHIFT chords. */
    [0x4F] = CS(4, 2),         /* RIGHT = CS+8 */
    [0x50] = CS(3, 4),         /* LEFT  = CS+5 */
    [0x51] = CS(4, 4),         /* DOWN  = CS+6 */
    [0x52] = CS(4, 3),         /* UP    = CS+7 */

    /* CAPS LOCK on host = CS+2 on Speccy (toggle CAPS LOCK). */
    [0x39] = CS(3, 1),

    /* Numeric keypad — same Speccy keys as the top-row digits.
     * Useful for keyboards with a numpad even if the user uses it
     * for digits; also catches non-US layouts where the alpha-row
     * digit keys may come through with non-standard shifted keysyms
     * that RVVM happens to map to KP* HID usages (e.g. XKB_KEY_plus
     * → HID_KEY_KPPLUS). */
    [0x59] = DIGIT(3, 0),  /* KP1 */
    [0x5A] = DIGIT(3, 1),  /* KP2 */
    [0x5B] = DIGIT(3, 2),  /* KP3 */
    [0x5C] = DIGIT(3, 3),  /* KP4 */
    [0x5D] = DIGIT(3, 4),  /* KP5 */
    [0x5E] = DIGIT(4, 4),  /* KP6 */
    [0x5F] = DIGIT(4, 3),  /* KP7 */
    [0x60] = DIGIT(4, 2),  /* KP8 */
    [0x61] = DIGIT(4, 1),  /* KP9 */
    [0x62] = DIGIT(4, 0),  /* KP0 */
    [0x58] = PLAIN(6, 0),  /* KP_ENTER */

    /* Symbol HID usages — RVVM maps `+` keysym → KPPLUS regardless
     * of layout, so include the keypad-symbol set too. */
    [0x57] = SYM(6, 2, 6, 2),  /* KPPLUS    → SS+K (+) */
    [0x56] = SYM(6, 3, 4, 0),  /* KPMINUS   → SS+J (-) / shifted SS+0 (_) */
    [0x55] = SYM(7, 4, 7, 4),  /* KPASTRSK  → SS+B (*) */
    [0x54] = SYM(0, 4, 0, 4),  /* KPSLASH   → SS+V (/) */
    [0x67] = SYM(6, 1, 6, 2),  /* KPEQUAL   → SS+L (=) / SS+K (+) */

    /* Modifier keys themselves: NOT in this table. They're handled
     * specially by speccy_hid_event so it can track shift state and
     * synthesise chord prefixes. The table never returns a shift-key
     * cell for the modifier usages — they're left as UNDEF. */
};

bool keyboard_translate(uint8_t usage, bool host_shift_held,
                        kbd_chord_t *out) {
    if (usage >= sizeof(kbd_table) / sizeof(kbd_table[0])) return false;
    const key_def_t *d = &kbd_table[usage];
    if (!d->defined) return false;
    *out = host_shift_held ? d->shifted : d->plain;
    return true;
}
