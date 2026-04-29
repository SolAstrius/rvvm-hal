HAL      := ../..
PICOLIBC := $(HAL)/vendor/picolibc-build/min/install
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null || \
                    echo /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64)

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -fdata-sections \
            -DHAL_PICOLIBC \
            -isystem $(PICOLIBC)/include \
            -I$(HAL)/include

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld -Wl,--gc-sections

OBJS := build/main.o

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(PICOLIBC)/lib/libc.a:
	$(MAKE) -C $(HAL) picolibc-min

$(HAL)/libhal.a: $(PICOLIBC)/lib/libc.a
	$(MAKE) -C $(HAL) HAL_PICOLIBC=min

firmware.elf: $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

# RVVM's user-mode tap is on by default (omit -nonet). The
# default network is 10.0.2.0/24 (gateway 10.0.2.2, guest 10.0.2.15).
run: firmware.bin
	$(RVVM) firmware.bin -nogui -nosound

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
