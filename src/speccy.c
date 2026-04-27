#include "speccy.h"
#include <stddef.h>

/* External freestanding helpers from rvvm-hal/src/string.c. */
void *memset(void *dst, int c, unsigned long n);

/* Defined in keyboard.c. */
#include "keyboard.h"

/* Audio HAL — beeper edge channel + cumulative T-state counter.
 *
 * The beeper is bit 4 of the byte written to port 0xFE. We feed
 * audio_edge with the level changes timestamped in absolute T-states
 * (since boot, monotonically increasing). audio_edge converts to host
 * samples internally.
 *
 * total_t_states survives speccy_reset on purpose — the audio cycle
 * timeline must stay monotonic across in-firmware resets, otherwise
 * audio_edge_set would receive a cycle older than its origin and the
 * resampler math goes negative. */
#include "audio.h"
#include "audio_edge.h"

#define SPECCY_Z80_HZ      3500000ULL    /* 48K timing; 128K is 3.5469 MHz
                                            but the audio resampler tolerates
                                            the ~0.5% difference inaudibly */
#define BEEPER_LEVEL_HIGH    0x4000
#define BEEPER_LEVEL_LOW    -0x4000

static audio_edge_t beeper;
static bool         beeper_open       = false;
static uint64_t     total_t_states    = 0;
static uint64_t     frame_start_cycle = 0;

/* Tweakable from debug.c — when true, speccy_step_frame skips the
 * Z80Interrupt call so we can observe the Z80 main loop without the
 * IRQ vector dominating PC samples. */
bool _debug_irq_disabled = false;

bool speccy_audio_init(void) {
    if (!audio_init()) return false;
    if (!audio_edge_open(&beeper, SPECCY_Z80_HZ)) return false;
    beeper_open = true;
    return true;
}

void speccy_apply_paging(speccy_t *vm) {
    if (vm->rom_idx >= SPECCY_NUM_ROM_BANKS) vm->rom_idx = 0;
    if (vm->ram_idx >= SPECCY_NUM_RAM_BANKS) vm->ram_idx = 0;
    vm->page_0000 = vm->rom[vm->rom_idx];
    vm->page_4000 = vm->ram[5];   /* always */
    vm->page_8000 = vm->ram[2];   /* always */
    vm->page_C000 = vm->ram[vm->ram_idx];
}

void speccy_reset(speccy_t *vm, bool is_128k) {
    /* Zero RAM but DON'T zero ROM — the firmware loads it before reset. */
    memset(vm->ram, 0, sizeof(vm->ram));

    vm->mode_128k      = is_128k;
    vm->t_per_frame    = is_128k ? SPECCY_T_PER_FRAME_128K
                                 : SPECCY_T_PER_FRAME_48K;
    vm->rom_idx        = 0;       /* 128K editor on 128K, 48K BASIC on 48K */
    vm->ram_idx        = 0;
    vm->screen_idx     = 5;       /* normal screen */
    vm->paging_locked  = !is_128k;   /* 48K mode = paging locked from boot */
    vm->port_7ffd      = 0;
    speccy_apply_paging(vm);

    /* All keys up — the matrix is active-low. Bits 5..7 stay set
     * because the ULA only owns bits 0..4. */
    for (int i = 0; i < 8; i++) vm->kbd[i] = 0xFF;
    for (int i = 0; i < 8 * 5; i++) vm->release_latch[i] = 0;
    vm->hid_head     = 0;
    vm->hid_tail     = 0;
    vm->gap_frames   = 0;
    vm->last_released_packed = 0xFF;
    vm->border       = 7;          /* white border on power-on */
    vm->beeper       = 0;
    vm->fb_dirty     = true;       /* force first render */
    vm->frame_count  = 0;
    vm->pre_irq_pc   = 0;
    Z80Reset(&vm->z80);
}

/* Hold each released key down for N frames so BASIC's IRQ-driven
 * keyboard scan sees it. The 48K ROM's KEYBOARD-INPUT uses a debounce
 * counter that starts at 5 and decrements every IRQ-frame the same
 * key is held; only when it hits 0 does the press get dispatched. */
#define KEY_RELEASE_LATCH  5

/* After a key is fully released (latch hits 0), wait this many
 * additional "all-up" frames before draining the next event from the
 * HID queue. BASIC's KSTATE retains the previous key in its
 * debounce/repeat tracking for several frames after release; if we
 * dispatch the next press too soon BASIC treats the press as part of
 * the previous keystroke's repeat sequence and refuses to dispatch
 * it as a new keystroke. */
#define KEY_RELEASE_GAP   3

/* HID queue helpers. Encoding: bit 7 = action (0=DOWN, 1=UP),
 * bits 6..3 = row (0..7), bits 2..0 = col (0..4). */
#define HID_QSIZE  ((int)sizeof(((speccy_t*)0)->hid_queue))

static inline uint8_t hid_pack(uint8_t row, uint8_t col, bool down) {
    return (uint8_t)((down ? 0 : 0x80) | ((row & 7) << 3) | (col & 7));
}
static inline void hid_unpack(uint8_t v, uint8_t *row, uint8_t *col, bool *down) {
    *down = (v & 0x80) == 0;
    *row  = (v >> 3) & 7;
    *col  = v & 7;
}

void speccy_kbd_enqueue(speccy_t *vm, uint8_t row, uint8_t col, bool pressed) {
    uint8_t next = (uint8_t)((vm->hid_tail + 1) % HID_QSIZE);
    if (next == vm->hid_head) return;   /* full — drop */
    vm->hid_queue[vm->hid_tail] = hid_pack(row, col, pressed);
    vm->hid_tail = next;
}

/* How many events are queued (used by debug.c to pace UART input
 * expansion against the speccy core's drain rate). */
uint8_t _speccy_hid_pending(speccy_t *vm) {
    return (uint8_t)((vm->hid_tail - vm->hid_head + HID_QSIZE) % HID_QSIZE);
}

/* Called once per frame from speccy_step_frame: tick release latches,
 * then drain at most one HID queue entry. The "one event per frame"
 * pacing is what makes repeated taps of the same key register as
 * distinct keystrokes — without it, an HID DOWN-UP-DOWN burst within
 * a single frame would just look like "still pressed" to BASIC. */
static void tick_release_latches(speccy_t *vm) {
    /* Snapshot whether any key was mid-release at the START of this
     * tick. If yes, we won't drain new events even after decrement —
     * we want BASIC's KEY-SCAN to see at least one full frame of
     * "all up" before the next press. Otherwise a DOWN drained in
     * the same tick where the previous release latch hit 0 would
     * make BASIC see continuous press across frames. */
    bool any_latched_before = false;
    for (int i = 0; i < 8 * 5; i++) {
        if (vm->release_latch[i] > 0) { any_latched_before = true; break; }
    }

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 5; col++) {
            uint8_t *latch = &vm->release_latch[row * 5 + col];
            if (*latch == 0) continue;
            if (--*latch == 0) {
                vm->kbd[row] |= (uint8_t)(1u << col);
            }
        }
    }

    /* If a release just completed in this tick, remember which key
     * was last released and start a gap window. The gap is only
     * enforced if the NEXT press is the SAME key (KSTATE auto-repeat
     * suppression); pressing a different key can drain immediately. */
    if (any_latched_before) {
        bool any_latched_now = false;
        for (int i = 0; i < 8 * 5; i++) {
            if (vm->release_latch[i] > 0) { any_latched_now = true; break; }
        }
        if (!any_latched_now) {
            /* All keys just went up. Start the gap window — the
             * SAME key being re-pressed within KEY_RELEASE_GAP frames
             * waits; different keys drain immediately. */
            vm->gap_frames = KEY_RELEASE_GAP;
        }
        return;
    }

    if (vm->hid_head == vm->hid_tail) {
        if (vm->gap_frames > 0) vm->gap_frames--;
        return;
    }

    /* Peek the next event to decide whether the gap applies. */
    uint8_t row, col;
    bool    pressed;
    hid_unpack(vm->hid_queue[vm->hid_head], &row, &col, &pressed);

    if (vm->gap_frames > 0) {
        uint8_t pk = (uint8_t)((row << 3) | col);
        if (pressed && pk == vm->last_released_packed) {
            /* Same key being re-pressed during the gap: hold off. */
            vm->gap_frames--;
            return;
        }
        /* Different key (or a release event): drain immediately,
         * the gap was specific to the previous key. */
        vm->gap_frames = 0;
    }

    vm->hid_head = (uint8_t)((vm->hid_head + 1) % HID_QSIZE);
    if (!pressed) {
        vm->last_released_packed = (uint8_t)((row << 3) | col);
    }
    speccy_set_key(vm, row, col, pressed);

    /* DOWN+UP coalescing: if the next event is the matching UP for
     * the key we just pressed, drain it now too. The release latch
     * then holds the matrix bit cleared so BASIC still sees the
     * press for KEY_RELEASE_LATCH IRQ frames. Halves per-keystroke
     * latency by eliminating one drain frame per char (DOWN at
     * frame N, UP also at N, instead of UP at N+1). */
    if (pressed && vm->hid_head != vm->hid_tail) {
        uint8_t row2, col2;
        bool    pressed2;
        hid_unpack(vm->hid_queue[vm->hid_head], &row2, &col2, &pressed2);
        if (!pressed2 && row2 == row && col2 == col) {
            vm->hid_head = (uint8_t)((vm->hid_head + 1) % HID_QSIZE);
            vm->last_released_packed = (uint8_t)((row << 3) | col);
            speccy_set_key(vm, row, col, false);
        }
    }
}

uint32_t speccy_step_frame(speccy_t *vm) {
    /* Decay any pending HID release latches before running the
     * frame, so newly-pressed keys are guaranteed to be observable
     * by the IRQ-time keyboard scan that runs inside Z80Emulate. */
    tick_release_latches(vm);

    /* Snapshot the cycle counter at frame start. speccy_out reads
     * this when stamping audio events: absolute_cycle =
     * frame_start_cycle + cycles_in_frame (the latter is z80emu's
     * elapsed_cycles passed through the OUT macro). */
    frame_start_cycle = total_t_states;

    /* Run a frame's worth of T-states, then raise the vblank IRQ.
     * z80emu's "interrupt" path injects the bus value (0xFF for IM 1
     * or as the low half of the IM 2 vector); the Speccy ULA puts
     * 0xFF on the bus and the ROM uses IM 1 by default, so 0xFF is
     * the correct payload either way. */
    uint32_t executed = (uint32_t)Z80Emulate(&vm->z80,
                                             vm->t_per_frame,
                                             vm);
    total_t_states += executed;

    /* Snapshot PC for diagnostics — right after Z80Emulate, before
     * Z80Interrupt potentially clobbers it with 0x0038. */
    vm->pre_irq_pc = (uint16_t)vm->z80.pc;

    if (!_debug_irq_disabled) {
        Z80Interrupt(&vm->z80, 0xFF, vm);
    }

    /* Render this frame's audio. Cheap if no beeper writes happened —
     * audio_edge fills the whole window with the holding level. */
    if (beeper_open) audio_edge_advance(&beeper, total_t_states);

    vm->frame_count++;
    return executed;
}

uint8_t speccy_in(speccy_t *vm, uint16_t port) {
    /* ULA decode: any port with bit 0 = 0. The high byte selects
     * which keyboard half-rows to read (active-low: bit n = 0 →
     * scan row n). Multiple selected rows AND together — that's how
     * the ROM's "any key" scan works. */
    if ((port & 1) == 0) {
        uint8_t row_select = (uint8_t)(~(port >> 8));
        uint8_t result     = 0xFF;
        for (int row = 0; row < 8; row++) {
            if (row_select & (1u << row)) {
                result &= vm->kbd[row];
            }
        }
        /* Bits 5,7 always read 1; bit 6 = EAR (tape input) — tied
         * high since we have no tape. Mask to bits 0..4 of matrix. */
        return (uint8_t)((result & 0x1F) | 0xA0);
    }
    /* Unmapped ports: float to 0xFF on real hardware. */
    return 0xFF;
}

void speccy_out(speccy_t *vm, uint16_t port, uint8_t value, uint32_t cycles) {
    /* ULA — any port with bit 0 = 0. */
    if ((port & 1) == 0) {
        vm->border = (uint8_t)(value & 0x07);
        uint8_t new_beeper = (uint8_t)((value >> 4) & 1);
        if (new_beeper != vm->beeper) {
            vm->beeper = new_beeper;
            if (beeper_open) {
                audio_edge_set(&beeper,
                               new_beeper ? BEEPER_LEVEL_HIGH : BEEPER_LEVEL_LOW,
                               frame_start_cycle + cycles);
            }
        }
        return;
    }

    /* 128K paging port 0x7FFD. Decode is "A15=0 AND A1=0" so any port
     * matching `xxxxxxxx-x---x-0` selects it; the canonical address is
     * 0x7FFD. We accept the canonical form plus the most common mirror
     * 0x1FFD (which is actually +3's 2nd paging port — see spec) by
     * matching A15=0 + A1=0 strictly. Once vm->paging_locked, writes
     * are silently dropped (per the lock bit's meaning). */
    if (vm->mode_128k && !vm->paging_locked
        && (port & 0x8002) == 0) {
        vm->port_7ffd     = value;
        vm->ram_idx       = (uint8_t)(value & SPECCY_7FFD_RAM_MASK);
        vm->screen_idx    = (value & SPECCY_7FFD_SCREEN) ? 7 : 5;
        vm->rom_idx       = (value & SPECCY_7FFD_ROM)    ? 1 : 0;
        if (value & SPECCY_7FFD_LOCK) vm->paging_locked = true;
        speccy_apply_paging(vm);
        /* Screen-bank toggle changes what the renderer should display
         * even if no RAM byte changed; force a redraw. */
        vm->fb_dirty = true;
    }
}

/* Host modifier state. Tracked across speccy_hid_event invocations so
 * we can pick the right chord for shifted keys. We DON'T emit a CAPS-
 * SHIFT press for the host's LSHIFT/RSHIFT directly — instead, the
 * chord lookup does it per-key (so SHIFT+digit gives "!@#" via
 * SYMBOL_SHIFT, while SHIFT+letter gives 'A' via CAPS_SHIFT). */
static struct {
    bool shift;   /* LSHIFT or RSHIFT */
    bool ctrl;    /* LCTRL  or RCTRL  → SYMBOL SHIFT alias */
    bool alt;     /* LALT   or RALT   → SYMBOL SHIFT alias */
} host_mods;

/* Per-HID-usage record of which Speccy chord we emitted on DOWN.
 * Used so that the matching UP releases EXACTLY the same chord —
 * even if host shift state changed in between. */
static struct {
    kbd_chord_t chord;
    uint8_t     active;
} active_keys[0x100];

static void enqueue_chord(speccy_t *vm, kbd_chord_t c, bool pressed) {
    if (pressed) {
        if (c.shift == KBD_CAPS_SHIFT)
            speccy_kbd_enqueue(vm, 0, 0, true);
        else if (c.shift == KBD_SYMBOL_SHIFT)
            speccy_kbd_enqueue(vm, 7, 1, true);
        speccy_kbd_enqueue(vm, c.row, c.col, true);
    } else {
        speccy_kbd_enqueue(vm, c.row, c.col, false);
        if (c.shift == KBD_CAPS_SHIFT)
            speccy_kbd_enqueue(vm, 0, 0, false);
        else if (c.shift == KBD_SYMBOL_SHIFT)
            speccy_kbd_enqueue(vm, 7, 1, false);
    }
}

bool speccy_hid_event(speccy_t *vm, uint8_t usage, bool pressed) {
    /* Modifier keys: track state, don't emit a Speccy press for the
     * modifier itself. The chord lookup adds the appropriate Speccy
     * shift per-key.
     *
     * Exception: LCTRL/RCTRL (and LALT/RALT) get aliased to SYMBOL
     * SHIFT directly — useful for users who want manual access to
     * Speccy's SS+key combos (entering BASIC tokens etc). */
    switch (usage) {
    case 0xE1: case 0xE5:   /* L/R SHIFT */
        host_mods.shift = pressed;
        return true;
    case 0xE0: case 0xE4:   /* L/R CTRL → SYMBOL SHIFT */
        host_mods.ctrl = pressed;
        speccy_kbd_enqueue(vm, 7, 1, pressed);
        return true;
    case 0xE2: case 0xE6:   /* L/R ALT → SYMBOL SHIFT alias */
        host_mods.alt = pressed;
        speccy_kbd_enqueue(vm, 7, 1, pressed);
        return true;
    case 0xE3: case 0xE7:   /* L/R META — unmapped on Speccy */
        return false;
    }

    if (pressed) {
        kbd_chord_t c;
        if (!keyboard_translate(usage, host_mods.shift, &c)) return false;
        active_keys[usage].chord  = c;
        active_keys[usage].active = 1;
        enqueue_chord(vm, c, true);
    } else {
        /* Use the chord we ACTUALLY emitted on DOWN — host shift
         * may have changed in the meantime. */
        if (!active_keys[usage].active) return false;
        kbd_chord_t c = active_keys[usage].chord;
        active_keys[usage].active = 0;
        enqueue_chord(vm, c, false);
    }
    return true;
}

void speccy_set_key(speccy_t *vm, uint8_t row, uint8_t col, bool pressed) {
    if (row >= 8 || col >= 5) return;
    if (pressed) {
        /* Force-release every key currently sitting in its release-
         * latch window. Bits with latch == 0 are either "all up" or
         * "actively held by host" — left alone. Bits with latch > 0
         * are stale leftovers from a recently-released key; if we
         * leave them BASIC's KEY-SCAN sees two keys held and treats
         * it as ambiguous (or, for shift+key, as a spurious shifted
         * variant of the new key). Dropping them here gives the new
         * press a clean matrix to land in.
         *
         * Active shift keys (held by the user across multiple typed
         * letters) are unaffected: their bits are cleared with
         * latch == 0, so the loop skips them. */
        for (int i = 0; i < 8 * 5; i++) {
            if (vm->release_latch[i] == 0) continue;
            int r = i / 5, c = i % 5;
            if (r == row && c == col) continue;
            vm->kbd[r] |= (uint8_t)(1u << c);
            vm->release_latch[i] = 0;
        }
        vm->kbd[row] &= (uint8_t)~(1u << col);
        vm->release_latch[row * 5 + col] = 0;
    } else {
        /* Defer the actual release: keep the bit cleared, set a latch
         * so it stays pressed for KEY_RELEASE_LATCH frames before
         * lifting. Real key-up is published when the latch decays. */
        vm->release_latch[row * 5 + col] = KEY_RELEASE_LATCH;
    }
}
