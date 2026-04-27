# scev-cores/zx-spectrum — bare-metal ZX Spectrum 48K emulator on RVVM.
#
# Consumes rvvm-hal (vendor/rvvm-hal as a git submodule) for the
# device drivers, vendors z80emu (anotherlin's permissive Z80 core)
# under vendor/. Only Speccy-specific glue lives in src/.
#
# Build: `make`              produces firmware.bin
# Run:   `make run`           boots under RVVM with -bochs_display + -hda_test
#        `make run-snap SNAP=…/foo.sna`   load a snapshot off a 2nd disk
# Clean: `make clean`
#
# ROM (roms/48.rom) is mandatory — it's loaded as the first NVMe disk.
# Speccy-specific code reads 16 KiB from LBA 0 into vm.mem before reset.

HAL      := vendor/rvvm-hal
Z80      := vendor/z80emu
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

# z80emu.c hardcodes `#include "z80user.h"`. We ship our own at
# src/z80user.h (the upstream sample is removed from vendor/z80emu/);
# `-Isrc` precedes `-I$(Z80)` so ours wins.
CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -Isrc -I$(HAL)/include -I$(Z80)

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld

# Speccy core objects.
OBJS     := build/main.o build/speccy.o build/render.o build/keyboard.o \
            build/snapshot.o build/debug.o build/z80emu.o

all: firmware.bin

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# z80emu lives under vendor/, so it doesn't pick up `src/%.c` rule.
build/z80emu.o: $(Z80)/z80emu.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

ROM ?= roms/48.rom

run: firmware.bin
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test -nvme $(ROM)

run-headless: firmware.bin
	$(RVVM) firmware.bin -nogui -nonet -hda_test -nvme $(ROM)

run-snap: firmware.bin
	@test -n "$(SNAP)" || { echo "usage: make run-snap SNAP=path/to/foo.sna"; exit 1; }
	$(RVVM) firmware.bin -bochs_display -nonet -hda_test -nvme $(ROM) -nvme $(SNAP)

clean:
	rm -rf build firmware.elf firmware.bin
	$(MAKE) -C $(HAL) clean

.PHONY: all run run-headless run-snap clean
