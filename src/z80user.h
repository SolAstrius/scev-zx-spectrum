/* z80emu user-callback macros wired into our speccy_t.
 *
 * z80emu.c hardcodes `#include "z80user.h"`; our Makefile puts `src/`
 * before `vendor/z80emu/` on the include path so this file wins.
 *
 * The macros here run inside the emulator's hot loop. We resolve
 * `context` (the void* the caller passed to Z80Emulate) to a
 * speccy_t* and read/write its mem[] array directly. ROM reads pass
 * through; ROM writes are silently dropped (as on real hardware).
 * I/O is delegated to speccy_in / speccy_out for cleanliness. */

#ifndef __Z80USER_INCLUDED__
#define __Z80USER_INCLUDED__

#ifdef __cplusplus
extern "C" {
#endif

#include "speccy.h"

#define _SPECCY ((speccy_t *)context)

/* Memory reads — same path for code fetch, operand fetch, and data. */
#define Z80_READ_BYTE(address, x) \
    do { (x) = _SPECCY->mem[(address) & 0xFFFF]; } while (0)

#define Z80_FETCH_BYTE(address, x) Z80_READ_BYTE((address), (x))

#define Z80_READ_WORD(address, x) \
    do { uint16_t _addr = (uint16_t)(address); \
         (x) = _SPECCY->mem[_addr] \
             | ((uint16_t)_SPECCY->mem[(uint16_t)(_addr + 1)] << 8); } while (0)

#define Z80_FETCH_WORD(address, x)         Z80_READ_WORD((address), (x))
#define Z80_READ_WORD_INTERRUPT(address, x) Z80_READ_WORD((address), (x))

/* Memory writes — drop ROM writes (addr < 0x4000) on the floor. The
 * Z80 doesn't fault on these; the original Speccy hardware just
 * pulled the write line on a ROM chip. */
#define Z80_WRITE_BYTE(address, x) \
    do { uint16_t _addr = (uint16_t)(address); \
         if (_addr >= SPECCY_ROM_SIZE) { \
             _SPECCY->mem[_addr] = (uint8_t)(x); \
             if (_addr >= SPECCY_SCREEN_BASE \
                 && _addr < SPECCY_ATTR_BASE + SPECCY_ATTR_SIZE) { \
                 _SPECCY->fb_dirty = true; \
             } \
         } } while (0)

#define Z80_WRITE_WORD(address, x) \
    do { Z80_WRITE_BYTE((address),     (x) & 0xFF); \
         Z80_WRITE_BYTE((address) + 1, ((x) >> 8) & 0xFF); } while (0)

#define Z80_WRITE_WORD_INTERRUPT(address, x) Z80_WRITE_WORD((address), (x))

/* I/O — defer to the speccy_t-aware helpers. */
#define Z80_INPUT_BYTE(port, x) \
    do { (x) = speccy_in(_SPECCY, (port)); } while (0)

#define Z80_OUTPUT_BYTE(port, x) \
    do { speccy_out(_SPECCY, (port), (x)); } while (0)

#ifdef __cplusplus
}
#endif

#endif
