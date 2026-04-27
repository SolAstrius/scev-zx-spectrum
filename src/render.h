/* ZX Spectrum 256×192 + attribute screen → linear XRGB8888 framebuffer. */
#pragma once
#include "speccy.h"
#include "gfx.h"

/* Render the current screen into the supplied gfx surface. Writes a
 * 2× scaled image with `border` cells of solid border colour around
 * the active 256×192 picture, anchored at (x0, y0). Touches all
 * (256*scale + 2*border*scale) × (192*scale + 2*border*scale) pixels.
 *
 * `scale` should be 1 or 2 — anything else works but isn't
 * particularly tuned. Clears speccy_t::fb_dirty when done. */
void speccy_render(speccy_t *vm, const gfx_t *g,
                   uint32_t x0, uint32_t y0, uint32_t scale);
