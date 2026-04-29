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
            -DHAL_PICOLIBC -DHAL_FATFS \
            -isystem $(PICOLIBC)/include \
            -I$(HAL)/include -I$(HAL)/vendor/fatfs

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld -Wl,--gc-sections

OBJS := build/main.o

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

$(PICOLIBC)/lib/libc.a:
	$(MAKE) -C $(HAL) picolibc-min

$(HAL)/libhal.a: $(PICOLIBC)/lib/libc.a
	$(MAKE) -C $(HAL) HAL_PICOLIBC=min HAL_FATFS=1

firmware.elf: $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a $(PICOLIBC)/lib/libc.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

# Run with the fs-hello exFAT image attached so 'Pick a file'
# has something to show. If it doesn't exist, the menu disables
# the file-picker option.
NVME_FLAG := $(if $(wildcard ../fs-hello/exfat.img),-nvme ../fs-hello/exfat.img,)

# Headless: UART backend only. Drive with arrow keys in the host terminal.
run: firmware.bin
	$(RVVM) firmware.bin -nogui -nonet -nosound $(NVME_FLAG)

# GUI mode: Bochs framebuffer pops up alongside the UART. The same
# menu renders to both, and you can drive it from EITHER the GUI
# window's keyboard OR the host terminal. Try typing in both.
run-gui: firmware.bin
	$(RVVM) firmware.bin -bochs_display -nonet -nosound $(NVME_FLAG)

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
