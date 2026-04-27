/* z80emu user-callback macros wired into our speccy_t.
 *
 * z80emu.c hardcodes `#include "z80user.h"`; our Makefile puts `src/`
 * before `vendor/z80emu/` on the include path so this file wins.
 *
 * The macros here run inside the emulator's hot loop. We resolve
 * `context` (the void* the caller passed to Z80Emulate) to a
 * speccy_t* and dispatch through bank-aware accessors so paging works
 * transparently. */

#ifndef __Z80USER_INCLUDED__
#define __Z80USER_INCLUDED__

#ifdef __cplusplus
extern "C" {
#endif

#include "speccy.h"

#define _SPECCY ((speccy_t *)context)

/* Memory reads — same path for code fetch, operand fetch, and data. */
#define Z80_READ_BYTE(address, x) \
    do { (x) = speccy_read8(_SPECCY, (uint16_t)(address)); } while (0)

#define Z80_FETCH_BYTE(address, x) Z80_READ_BYTE((address), (x))

#define Z80_READ_WORD(address, x) \
    do { uint16_t _addr = (uint16_t)(address); \
         (x) = speccy_read8(_SPECCY, _addr) \
             | ((uint16_t)speccy_read8(_SPECCY, (uint16_t)(_addr + 1)) << 8); \
       } while (0)

#define Z80_FETCH_WORD(address, x)         Z80_READ_WORD((address), (x))
#define Z80_READ_WORD_INTERRUPT(address, x) Z80_READ_WORD((address), (x))

/* Memory writes — speccy_write8 silently drops writes below 0x4000
 * (the ROM region) and dirties the framebuffer for screen-RAM hits. */
#define Z80_WRITE_BYTE(address, x) \
    do { speccy_write8(_SPECCY, (uint16_t)(address), (uint8_t)(x)); } while (0)

#define Z80_WRITE_WORD(address, x) \
    do { uint16_t _a = (uint16_t)(address); \
         speccy_write8(_SPECCY, _a,           (uint8_t)((x) & 0xFF)); \
         speccy_write8(_SPECCY, (uint16_t)(_a + 1), (uint8_t)(((x) >> 8) & 0xFF)); \
       } while (0)

#define Z80_WRITE_WORD_INTERRUPT(address, x) Z80_WRITE_WORD((address), (x))

/* I/O — defer to the speccy_t-aware helpers.
 *
 * Z80_OUTPUT_BYTE carries z80emu's local elapsed_cycles (T-states into
 * the current Z80Emulate call) so speccy_out can timestamp the audio
 * edge. The 3-arg form is a deliberate departure from upstream
 * z80emu's macro signature — see vendor/z80emu/ SCEV PATCH comments
 * at the OUT call sites. */
#define Z80_INPUT_BYTE(port, x) \
    do { (x) = speccy_in(_SPECCY, (port)); } while (0)

#define Z80_OUTPUT_BYTE(port, x, cycles) \
    do { speccy_out(_SPECCY, (port), (x), (uint32_t)(cycles)); } while (0)

#ifdef __cplusplus
}
#endif

#endif
