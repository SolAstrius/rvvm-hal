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

# Pre-format a 16 MiB exFAT image with a seed /hello.txt. Run once
# to bootstrap; thereafter the firmware appends to it.
fs-image:
	@command -v mkfs.exfat >/dev/null || { echo "needs exfatprogs"; exit 1; }
	dd if=/dev/zero of=exfat.img bs=1M count=16 status=none
	mkfs.exfat exfat.img >/dev/null
	mkdir -p .mnt
	@echo "(needs sudo to mount the image; if you'd rather not, edit fs-image to use mtools instead)"
	sudo mount -o loop exfat.img .mnt
	echo "first-line seed at $$(date -u +%FT%TZ)" | sudo tee .mnt/hello.txt >/dev/null
	sudo umount .mnt
	rmdir .mnt
	@printf 'fs-image: %s ready (%s bytes)\n' exfat.img "$$(stat -c %s exfat.img)"

run: firmware.bin
	@test -f exfat.img || { echo "missing exfat.img — run 'make fs-image' first"; exit 1; }
	$(RVVM) firmware.bin -nogui -nonet -nosound -nvme exfat.img

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run fs-image clean
