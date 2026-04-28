# picolibc-hello — smallest possible firmware that links against
# the HAL's vendored picolibc and calls real <stdio.h> printf.
#
# Two-step build:
#   1. The HAL itself must be built with HAL_PICOLIBC=min so that
#      libhal.a contains src/picolibc_hooks.c.
#   2. This firmware adds picolibc's headers + libc.a to its own
#      compile/link lines.
#
# Both steps are wrapped by the convenience targets in this file.

HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

PICOLIBC := $(HAL)/vendor/picolibc-build/min/install

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -DHAL_PICOLIBC \
            -isystem $(PICOLIBC)/include \
            -I$(HAL)/include

# --gc-sections trims unreferenced picolibc objects to the symbols
# the firmware actually uses. Without it, libc.a's full ~50 KiB lands
# in the binary even when we only call strlen.
LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld -Wl,--gc-sections

OBJS := build/main.o

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# The HAL's libhal.a must be built with HAL_PICOLIBC=min so that
# src/picolibc_hooks.c is included. Force that here — idempotent.
$(HAL)/libhal.a:
	$(MAKE) -C $(HAL) HAL_PICOLIBC=min

$(PICOLIBC)/lib/libc.a:
	$(MAKE) -C $(HAL) picolibc-min

firmware.elf: $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	$(RVVM) firmware.bin -nogui -nonet -nosound

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
