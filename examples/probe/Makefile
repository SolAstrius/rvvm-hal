# Reference: how to build a bare-metal RVVM firmware that consumes
# rvvm-hal as a sibling library. Real consumers would have HAL as a
# git submodule under vendor/ — here it's just two directories up.

HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

# zig cc + lld is LTO-aware, so we default to LTO on for examples — the
# linker drops unused HAL symbols beyond what `--gc-sections` alone
# catches, typically halving firmware.bin size. Override with
# `make HAL_LTO=` to disable. HAL_OPT mirrors the libhal.a knob.
HAL_OPT ?= s
HAL_LTO ?= 1

CFLAGS   := -O$(HAL_OPT) -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -I$(HAL)/include
ifeq ($(HAL_LTO),1)
CFLAGS    += -flto=full
LTOFLAGS  := -flto=full
else
LTOFLAGS  :=
endif

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld $(LTOFLAGS)

OBJS := build/main.o

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: $(HAL)/libhal.a
$(HAL)/libhal.a:
	$(MAKE) -C $(HAL) HAL_OPT=$(HAL_OPT) HAL_LTO=$(HAL_LTO)

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	rvvm firmware.bin -bochs_display -nonet -nosound

# Headless mode. RVVM gates `-bochs_display` behind `!nogui && !res`
# (see RVVM src/main.c:469), so passing `-nogui -bochs_display` silently
# skips Bochs creation and the firmware sees an empty PCI bus. We use
# `-res WxH -nogui` instead — that drives RVVM's simple-framebuffer
# init path which works without a GUI window. Same gfx surface from
# the firmware's POV via the simplefb backend.
run-headless: firmware.bin
	rvvm firmware.bin -res 640x400 -nogui -nonet -nosound

# QEMU virt: bare-metal M-mode kernel boot (no OpenSBI). Same binary
# RVVM runs — boots, FDT-walks, lists devices. The probe's later
# stages exercise RVVM-only devices (Bochs, opencores-i2c, NVMe,
# C-Media HDA, RTL8169, i2c-HID); on QEMU virt those paths just
# silently no-op since the FDT compatible strings aren't present.
QEMU     ?= qemu-system-riscv64
QEMU_CPU ?= rv64,zba=true,zbb=true,zbs=true,zicond=true,zicboz=true

run-qemu: firmware.elf
	$(QEMU) -M virt -nographic -cpu $(QEMU_CPU) -bios none -kernel firmware.elf

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run run-headless run-qemu clean
