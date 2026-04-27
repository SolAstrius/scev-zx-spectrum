/* Tiny UART debug surface. Polls host stdin every frame; characters
 * either inject as Speccy keystrokes or, if prefixed with the escape
 * char, run a debug command (dump registers, dump memory, etc).
 *
 * Goals:
 *   - Run the firmware without ever touching the GUI window — useful
 *     for SSH / headless / `make run-headless` workflows.
 *   - Drop a console for inspecting Z80 state when something looks
 *     wrong (stuck PC, RAM corruption, wrong IM, ...).
 *
 * Escape char: backtick (`). Followed by one of:
 *   d     dump Z80 registers + flags + IM/IFF/border
 *   m XX  dump 256 bytes starting at hex address XX (lowercase, 1-4
 *         hex digits), e.g. `m 4000` for the start of screen RAM
 *   p     toggle periodic PC dump (off by default)
 *   r     reset the Speccy core (no ROM reload)
 *   h     this help
 *
 * Anything else is treated as a key press: we look up the printable
 * char in a small ASCII→(row,col) table, set the matrix bit, and on
 * the NEXT frame clear it (auto-repeat). For shifted symbols we
 * additionally hold the appropriate shift key for one frame. */

#pragma once
#include "speccy.h"

void debug_init(speccy_t *vm);
void debug_poll(speccy_t *vm, uint32_t frame_count);
