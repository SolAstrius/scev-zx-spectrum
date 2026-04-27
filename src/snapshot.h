/* Snapshot loaders. .sna and .z80 v1 for v1 firmware. */
#pragma once
#include "speccy.h"

/* Sniff and load. `buf` should be the first ~64 KB of an attached
 * disk image (we read enough sectors via ATA in main.c). Returns
 * true on success, false if no recognised format was found. */
bool snapshot_load(speccy_t *vm, const uint8_t *buf, uint32_t len);
