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
            -DHAL_PICOLIBC -DHAL_LWIP \
            -isystem $(PICOLIBC)/include \
            -I$(HAL)/include \
            -I$(HAL)/vendor/lwip-config \
            -I$(HAL)/vendor/lwip/src/include

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld -Wl,--gc-sections

OBJS := build/main.o

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(PICOLIBC)/lib/libc.a:
	$(MAKE) -C $(HAL) picolibc-min

$(HAL)/libhal.a: $(PICOLIBC)/lib/libc.a
	$(MAKE) -C $(HAL) HAL_PICOLIBC=min HAL_LWIP=1

firmware.elf: $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

# Default test: just see DHCP complete.
run: firmware.bin
	$(RVVM) firmware.bin -nogui -nosound

# Forwarded test: udp port 2007 on the host → port 7 in guest.
# Try `echo hi | nc -u -w1 127.0.0.1 2007` from another terminal.
run-portfwd: firmware.bin
	$(RVVM) firmware.bin -nogui -nosound -portfwd udp/2007=7

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run run-portfwd clean
