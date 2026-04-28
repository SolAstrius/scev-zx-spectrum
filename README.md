# scev-cores/zx-spectrum

A ZX Spectrum 48K emulator running as **bare-metal RISC-V firmware** under
[RVVM](https://github.com/LekKit/RVVM). No SBI, no kernel — the
firmware boots straight into M-mode, brings up its own device stack,
copies the Sinclair ROM into Z80 memory, and runs the machine at
50 Hz against RVVM's PCI display, I²C HID keyboard, and ATA disk.

`firmware.bin` is ~50 KB. About 1,500 lines of C in `src/` do the
Speccy-specific work; the rest is vendored.

## Layout

```
src/                 Speccy core (everything in this repo's git history)
  main.c               boot, frame loop, snapshot sniff
  speccy.c/.h          Z80 wrapper: ULA port, IRQ pacing, kbd matrix, HID queue
  render.c/.h          interleaved bitmap → linear XRGB8888, BRIGHT + FLASH
  keyboard.c/.h        USB HID usage → Speccy 8×5 matrix mapping (incl. chords)
  snapshot.c/.h        .sna loader (49,179-byte format, post-IRQ PC)
  debug.c/.h           UART console: screenshot, regdump, mem peek, key-inject
  z80user.h            z80emu callback macros — wired to speccy_t.mem[] / I/O
  rom_48k.h            16 KB Sinclair 48K ROM, embedded as a C array

vendor/rvvm-hal      git submodule → SolAstrius/rvvm-hal (sister repo)
vendor/z80emu        copied-in (not a submodule), see "vendoring" below
roms/                empty; .sna files for `make run-snap` go here or anywhere
tests/               Python harness — drives RVVM over UART, asserts on
                     screen / register / matrix state
flake.nix            zig + llvm-bintools + libasound for `make run`
```

## Vendored code

### `vendor/rvvm-hal/` — git submodule

[SolAstrius/rvvm-hal](https://github.com/SolAstrius/rvvm-hal). My own
HAL for RVVM bare-metal firmware: NS16550A UART, FDT walker, PCI ECAM
scanner, Bochs Display, OpenCores I²C, HID-over-I²C keyboard, ATA PIO,
plus mtime/MMIO/string utilities and a linker script. Pinned by
submodule SHA. Built standalone into `libhal.a` and statically linked.

**No local modifications** — this repo consumes it as a clean
dependency. Bug fixes go upstream.

### `vendor/z80emu/` — copied in (not a submodule)

[anotherlin/z80emu](https://github.com/anotherlin/z80emu) v1.1.3 by
Lin Ke-Fong, MIT-style permissive license (*"This code is free, do
whatever you want with it"*). Six files: `z80emu.c/.h`,
`z80config.h`, `instructions.h`, `macros.h`, `tables.h`. Compiled
into `build/z80emu.o` via the Makefile.

**Local modifications:**
- **`z80user.h` is ours**, not upstream's. Upstream ships a sample
  `z80user.h` for the zextest CP/M harness; we deleted it and put
  our Speccy-aware version in `src/z80user.h`. The Makefile orders
  `-Isrc` before `-I$(Z80)` so ours is the one z80emu's hardcoded
  `#include "z80user.h"` resolves to.
- **Patch to `z80emu.c`**: every `IN`/`OUT` opcode form now exposes the
  full 16-bit port to `Z80_INPUT_BYTE`/`Z80_OUTPUT_BYTE` instead of just
  the low byte. The real Z80 places A on bus bits 15:8 during `IN/OUT
  (n),A` and places BC on the full bus during `IN/OUT r,(C)` and the
  block I/O variants — stock z80emu drops the high half. Eight sites,
  one logical change, all tagged `/* SCEV PATCH */`:

  | Opcode | Old port | New port |
  |---|---|---|
  | `IN A,(n)` / `OUT (n),A`     | `n`  | `(A << 8) \| n` |
  | `IN r,(C)` / `OUT (C),r`     | `C`  | `BC` |
  | `INI` `IND` `INIR` `INDR`    | `C`  | `BC` |
  | `OUTI` `OUTD` `OTIR` `OTDR`  | `C`  | `BC` |

  **Plus a 3rd argument on the OUT macro** — every `Z80_OUTPUT_BYTE`
  call site additionally passes z80emu's local `elapsed_cycles` (T-
  states into the current `Z80Emulate` call), which `speccy_out` uses
  to timestamp beeper-bit changes for the audio HAL's edge tracker.
  The 3-arg signature is a deliberate departure from upstream's macro;
  the alternative is reconstructing per-cycle timing from `Z80_*` macro
  call frequency, which is both lossy and more invasive.

  Why it matters: the Speccy ULA decodes keyboard reads on A8–A15 as
  the row-select mask (`speccy.c` does `row_select = ~(port >> 8)`).
  Without the patch `port >> 8` is always zero, the firmware sees "every
  row simultaneously selected", BASIC's KEYBOARD-SCAN gets garbage and
  refuses to dispatch keystrokes — the emulator boots but is
  functionally a brick. Commit `5b5ec60`.

### `src/rom_48k.h` — Sinclair 48K ROM

The 16 KB Sinclair ZX Spectrum 48K ROM, embedded as a C array via
`hxtools bin2c`. Bytes lifted from
[fruit-bat/pico-zxspectrum](https://github.com/fruit-bat/pico-zxspectrum).
Amstrad (the rights-holder) has long permitted free redistribution
of this ROM for emulator use.

## Build

Toolchain: `zig cc -target riscv64-freestanding-none` plus
`llvm-objcopy`. Both come from the Nix flake:

```sh
direnv allow         # or: nix develop
make                 # → firmware.bin (~50 KB)
make run             # boot under RVVM with the bochs display
make run-snap SNAP=foo.sna       # mount a snapshot via -ata
make run-headless    # no GUI, UART console only — useful for tests
```

The flake wires `LD_LIBRARY_PATH` for `libasound` and points
`ALSA_PLUGIN_DIR` at PipeWire's alsa-lib bridge so RVVM's HDA backend
finds an audio device on NixOS. The Speccy beeper (port `$FE` bit 4)
is fed into rvvm-hal's `audio_edge` primitive: every Z80 OUT to a ULA
port stamps a level change in T-states, and once per frame the
firmware tells the HAL to render samples up to the current cycle. No
build flag — it's on whenever `make run`'s `-hda_test` succeeds.

## Running

`make run` launches RVVM with `-bochs_display -nonet -hda_test`. The
firmware:

1. Walks the FDT, brings up UART / PCI / I²C / HID / ATA / display.
2. Resets the Z80, copies `rom_48k` into `mem[0..0x3FFF]`.
3. If anything was attached via `-ata`, sniffs the first sector for
   a `.sna` snapshot and loads it (replacing the post-ROM state).
4. Runs the frame loop: poll HID → drain one queued event → run
   69,888 T-states → fire vblank IRQ → render dirty bitmap.

Keyboard mapping in `src/keyboard.c` covers full QWERTY plus the
common chords: `Backspace` → CAPS+0, arrows → CAPS+5..8, `,` `.` `;`
`:` `"` `?` `!` etc. via SYM-SHIFT, with same-frame DOWN+UP
coalescing and a per-same-key gap so fast typing doesn't drop
characters.

## Debug surface

The firmware listens on the UART for single-keystroke commands
(see `src/debug.c`). Plain ASCII chars are queued as Speccy
keystrokes; backtick-prefixed chars are debug ops:

| key  | what it does |
|---|---|
| ` ` v` | 24×32 ASCII screenshot of the Speccy display |
| ` ` m X` | hex-dump 256 bytes from address X |
| ` ` d` | Z80 register dump |
| ` ` k` | keyboard matrix state + sysvar peek |

`tests/harness.py` drives this surface to write integration tests
without touching the GUI (see `test_typing_flow.py` for an example
that boots BASIC and types `PRINT 42`).

## Test it

```sh
make                                       # build firmware.bin first
nix develop -c python -m pytest tests/     # or just `pytest tests/` in shell
```

Each test spawns its own headless RVVM instance.

## License

- `src/` — public-domain reference code (treat as such).
- `vendor/rvvm-hal/` — MIT-ish, see the submodule's README.
- `vendor/z80emu/` — Lin Ke-Fong's permissive license (header
  comments).
- `src/rom_48k.h` — Sinclair / Amstrad; redistributable for
  emulator use.

RVVM itself isn't redistributed here; install it separately from
[LekKit/RVVM](https://github.com/LekKit/RVVM).
