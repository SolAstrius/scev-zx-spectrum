/* scev-cores/zx-spectrum — bare-metal ZX Spectrum 48K on RVVM.
 *
 * Boot sequence:
 *   1. Standard rvvm-hal init: UART, FDT walk, PCI/I2C base discovery.
 *   2. Bring up gfx (auto-selects bochs / simplefb).
 *   3. Bring up HID keyboard via i2c-hid.
 *   4. Reset Z80, copy 48K ROM into mem[0..0x3FFF].
 *   5. If RVVM was started with -ata <something>, sniff first sector
 *      for a snapshot and load it (replacing the post-ROM machine state).
 *   6. Frame loop: poll HID, run one PAL frame of T-states, render.
 *
 * Audio (HDA PCM streaming of the beeper) is wired but disabled by
 * default — turn it on with `-DAUDIO_ENABLED` once we trust the
 * Speccy step path. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "i2c.h"
#include "hid.h"
#include "nvme.h"
#include "gfx.h"
#include "rvvm.h"

#include "speccy.h"
#include "render.h"
#include "snapshot.h"
#include "debug.h"

#include <stddef.h>

extern char __bss_start[], __bss_end[];

void *memcpy(void *dst, const void *src, unsigned long n);

#define DISPLAY_SCALE     2
#define DISPLAY_W         ((SPECCY_SCREEN_W + 2 * SPECCY_BORDER) * DISPLAY_SCALE)
#define DISPLAY_H         ((SPECCY_SCREEN_H + 2 * SPECCY_BORDER) * DISPLAY_SCALE)

static speccy_t       vm;
static gfx_t          g;
static hid_keyboard_t kb;

static void on_key(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    /* Trace EVERY HID event with full mapping info — when something
     * isn't echoing in BASIC, the first question is "did the keypress
     * even reach the firmware?" so we log it unconditionally. */
    bool mapped = speccy_hid_event(&vm, usage, pressed);
    uart_printf("hid: usage=0x%x %s%s\n",
                (uint64_t)usage, pressed ? "DOWN" : "up  ",
                mapped ? "" : " (UNMAPPED)");
}

static uintptr_t fdt_addr_of(const fdt_t *fdt, const char *compat,
                             uintptr_t fallback) {
    uint32_t off = fdt_find_compatible(fdt, compat);
    if (off == UINT32_MAX) return fallback;
    uint64_t addr = 0;
    if (!fdt_node_reg64(fdt, off, 0, &addr, NULL)) return fallback;
    return (uintptr_t)addr;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\nscev-cores/zx-spectrum — ZX Spectrum 48K on RVVM\n");
    uart_printf("hartid=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* FDT discovery + driver re-init with the real addresses. */
    fdt_t fdt;
    bool fdt_ok = fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr);
    if (fdt_ok) {
        uart_init(fdt_addr_of(&fdt, "ns16550a",              RVVM_UART_BASE));
        pci_init(fdt_addr_of(&fdt, "pci-host-ecam-generic",  RVVM_PCI_ECAM_BASE));
        i2c_init(fdt_addr_of(&fdt, "opencores,i2c-ocores",   RVVM_I2C_OC_BASE));
        uint32_t cpus_off = fdt_find_node_named(&fdt, "cpus");
        uint32_t hz = 0;
        if (cpus_off != UINT32_MAX) fdt_node_prop_u32(&fdt, cpus_off, "timebase-frequency", &hz);
        time_init(fdt_addr_of(&fdt, "sifive,clint0",         RVVM_CLINT_BASE), hz);
    } else {
        uart_puts("warn: FDT invalid, using rvvm.h fallback addresses\n");
        pci_init(0);
        i2c_init(RVVM_I2C_OC_BASE);
        time_init(RVVM_CLINT_BASE, 0);
    }
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);

    /* Graphics. We mode-set bochs (or accept simplefb) at exactly
     * 320×256 × 2 = 640×512 — covers the picture + 32-pixel border. */
    bool have_gfx = gfx_init_fdt(&g, &fdt, DISPLAY_W, DISPLAY_H);
    bool db_gfx   = false;
    if (have_gfx) {
        /* Paint the whole surface in the power-on border colour
         * (white) so the unused area outside our window — under
         * simplefb where the surface may be larger than DISPLAY_W×H
         * — is sane rather than BSS-grey. */
        gfx_fill(&g, 0x00CDCDCD);
        /* Page-flip via Bochs Y_OFFSET — host display only ever sees
         * complete frames, no mid-render tearing. Both halves filled
         * so the area outside the picture stays power-on-white across
         * flips (only relevant on simplefb where g may exceed DISPLAY). */
        if (gfx_enable_double_buffer(&g)) {
            db_gfx = true;
            gfx_fill(&g, 0x00CDCDCD);   /* fills back */
            gfx_flip(&g);
            gfx_fill(&g, 0x00CDCDCD);   /* fills new back */
        }
        uart_printf("gfx: framebuffer up%s\n",
                    db_gfx ? " (double-buffered)" : "");
    } else {
        uart_puts("gfx: no display backend, running blind\n");
    }

    /* Power-on Speccy + load ROM from -nvme controller 0.
     *
     * The ROM is required — it's the entry point the Z80 jumps to at
     * reset. ROM size determines the boot mode:
     *   16 KiB → 48K mode   (rom[0] = 48K BASIC; paging locked)
     *   32 KiB → 128K mode  (rom[0] = 128K editor, rom[1] = 48K BASIC)
     *
     * Read 64 LBAs (32 KiB) into a staging buffer; use the disk's
     * actual size to pick the mode. */
    static nvme_t  rom_disk;
    static uint8_t rom_buf[32768] __attribute__((aligned(NVME_PAGE_SIZE)));
    if (!nvme_init_nth(&rom_disk, 0)) {
        uart_puts("FATAL: ROM disk not attached. Start RVVM with -nvme roms/48.rom\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uint32_t rom_lbas = rom_disk.num_lbas;
    if (rom_lbas > 64) rom_lbas = 64;
    if (nvme_read(&rom_disk, 0, rom_buf, rom_lbas) != rom_lbas) {
        uart_puts("FATAL: ROM disk read failed\n");
        for (;;) __asm__ volatile ("wfi");
    }
    bool rom_is_128k = (rom_lbas == 64);   /* exactly 32 KiB */
    if (rom_lbas != 32 && !rom_is_128k) {
        uart_printf("FATAL: ROM disk is %u sectors, expected 32 (16 KiB) or 64 (32 KiB)\n",
                    rom_lbas);
        for (;;) __asm__ volatile ("wfi");
    }

    speccy_reset(&vm, rom_is_128k);
    memcpy(vm.rom[0], rom_buf, 16384);
    if (rom_is_128k) {
        memcpy(vm.rom[1], rom_buf + 16384, 16384);
        uart_puts("ROM: 128K (rom[0]=editor, rom[1]=48K BASIC)\n");
    } else {
        /* 48K mode: ROM 1 stays unused (paging is locked), but we
         * mirror to rom[1] anyway so a stray rom_idx flip doesn't
         * land on garbage. */
        memcpy(vm.rom[1], rom_buf, 16384);
        uart_puts("ROM: 48K\n");
    }

    /* Optional snapshot from -nvme controller 1 (NVMe device added second
     * on the command line). 49 KiB buffer covers .sna + slack; page-aligned
     * so NVMe can DMA into it directly. */
    static uint8_t disk_buf[49664] __attribute__((aligned(NVME_PAGE_SIZE)));
    static nvme_t  snap_disk;
    if (nvme_init_nth(&snap_disk, 1)) {
        uint32_t lbas = sizeof(disk_buf) / NVME_LBA_SIZE;
        if (lbas > snap_disk.num_lbas) lbas = snap_disk.num_lbas;
        uint32_t got = nvme_read(&snap_disk, 0, disk_buf, lbas);
        if (got > 0) {
            if (snapshot_load(&vm, disk_buf, got * NVME_LBA_SIZE)) {
                uart_puts("snapshot loaded.\n");
            } else {
                uart_puts("disk 1 present but no recognised snapshot format\n");
            }
        }
    }

    debug_init(&vm);

    /* Audio: bring up HDA + open the beeper edge channel. Failure
     * (no -hda_test on the rvvm command line) is non-fatal — Speccy
     * just runs silently. */
    if (speccy_audio_init(&vm)) {
        uart_puts("audio: beeper online (port $FE bit 4 → audio_edge)\n");
    } else {
        uart_puts("audio: backend unavailable, running silently\n");
    }

    uart_printf("Booting %s...\n\n", rom_is_128k ? "128K editor" : "Sinclair BASIC");
    uint32_t x_off = (have_gfx && g.width  > DISPLAY_W) ? (g.width  - DISPLAY_W) / 2 : 0;
    uint32_t y_off = (have_gfx && g.height > DISPLAY_H) ? (g.height - DISPLAY_H) / 2 : 0;

    /* Pace at the Speccy's actual 50 Hz, NOT the HAL's hz/60 default.
     * Each speccy_step_frame advances the emulator by one PAL frame's
     * worth of T-states (= 1/50 s of emulator time); audio_edge
     * generates exactly that many host samples per call. If wall-clock
     * paces faster than 50 Hz, the audio ring overfills and the
     * advance() wait kicks in, causing periodic clicks. */
    const uint64_t ticks_per_frame = RVVM_TIME_HZ / SPECCY_FPS;
    uint64_t deadline = time_now() + ticks_per_frame;
    for (;;) {
        hid_kb_poll(&kb, on_key, NULL);
        debug_poll(&vm, (uint32_t)vm.frame_count);
        speccy_step_frame(&vm);
        if (have_gfx) {
            speccy_render(&vm, &g, x_off, y_off, DISPLAY_SCALE);
            if (db_gfx) gfx_flip(&g);
        }
        time_busy_until(deadline);
        deadline += ticks_per_frame;
    }
}
