/* USB HID usage code → ZX Spectrum keyboard matrix. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Translate a USB HID usage code into (row, col) on the Speccy's
 * 8×5 matrix. Returns false for unmapped keys. */
bool keyboard_translate(uint8_t usage, uint8_t *out_row, uint8_t *out_col);
