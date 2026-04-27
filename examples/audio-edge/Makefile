HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -I$(HAL)/include

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld

OBJS := build/main.o

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	$(RVVM) firmware.bin -nogui -nonet -hda_test

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
