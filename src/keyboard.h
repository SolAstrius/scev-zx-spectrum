/* USB HID usage code → ZX Spectrum keyboard matrix.
 *
 * The Speccy has 40 keys in 4 rows × 10:
 *
 *     1 2 3 4 5  6 7 8 9 0
 *     Q W E R T  Y U I O P
 *     A S D F G  H J K L ENTER
 *     CS Z X C V B N M SS SPACE
 *
 * Two SHIFTs with very different roles:
 *
 *   CAPS SHIFT (CS, row 0 col 0):
 *     - Capitalises letters (CS+A → 'A')
 *     - Edit / navigation modifier:
 *         CS+0 = DELETE      (host BACKSPACE)
 *         CS+5..8 = arrows   (host ←↓↑→)
 *         CS+SPACE = BREAK   (host ESC)
 *         CS+1 = EDIT
 *         CS+2 = CAPS LOCK
 *         CS+3 = TRUE VIDEO
 *         CS+4 = INV VIDEO
 *         CS+9 = GRAPH mode
 *
 *   SYMBOL SHIFT (SS, row 7 col 1):
 *     - Symbols on letter keys: SS+P=", SS+L==, SS+K=+, SS+J=-, SS+B=*,
 *       SS+V=/, SS+M=., SS+N=,, SS+R=<, SS+T=>, SS+O=;, SS+Z=:,
 *       SS+C=?, SS+W= AND token, etc.
 *     - Symbols on digit keys: SS+1=!, SS+2=@, SS+3=#, SS+4=$, SS+5=%,
 *       SS+6=&, SS+7=', SS+8=(, SS+9=), SS+0=_
 *
 * For a USB-keyboard user typing in our emulator we want the natural
 * mental model: pressing the host symbol key produces that symbol
 * (with the appropriate Speccy chord behind the scenes), pressing
 * SHIFT+something does what the host's printed legend says.
 *
 * That requires us to track host modifier state in the firmware and
 * pick between two mappings per HID usage. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    KBD_NO_SHIFT      = 0,
    KBD_CAPS_SHIFT    = 1,    /* row 0 col 0 */
    KBD_SYMBOL_SHIFT  = 2,    /* row 7 col 1 */
} kbd_shift_t;

/* One Speccy keystroke: a matrix cell plus an optional shift chord. */
typedef struct {
    uint8_t      row;
    uint8_t      col;
    kbd_shift_t  shift;
} kbd_chord_t;

/* Translate a USB HID usage code into a Speccy chord. `host_shift_held`
 * picks between the unshifted and host-shifted entries: e.g. HID `=`
 * with shift=false → "=" (SS+L); same usage with shift=true → "+"
 * (SS+K). For letters, host_shift_held simply adds CAPS SHIFT to the
 * chord. Returns false if the usage code isn't mapped. */
bool keyboard_translate(uint8_t usage, bool host_shift_held,
                        kbd_chord_t *out);
