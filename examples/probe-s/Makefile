# probe-s — S-mode payload, run by OpenSBI.
#
# Builds libhal.a with HAL_PRIV=s HAL_PLAT=s_sbi (the S-mode build)
# and links against link_s.ld (load address 0x80200000, matching
# RVVM's `-k` placement and OpenSBI fw_jump's default target).

HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

HAL_OPT ?= s
HAL_LTO ?= 1   # zig cc + lld is LTO-aware; HAL surface drops past --gc-sections

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

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link_s.ld $(LTOFLAGS)

OBJS := build/main.o

# OpenSBI binary — fw_jump variant for the `generic` platform. RVVM
# loads this as the M-mode firmware at 0x80000000; OpenSBI drops to
# S-mode and jumps to 0x80200000 (default for fw_jump on generic),
# matching where RVVM places `-k <payload>`.
#
# Tested with OpenSBI v1.4 fw_jump.bin. The qemu-vendored
# fw_dynamic.bin alternative produced no output under RVVM during
# bring-up — fw_dynamic expects a struct fw_dynamic_info pointer in
# a2 that RVVM doesn't fill in, and the fallback path doesn't reach
# UART init on the RVVM platform variant. fw_jump has no such
# dependency: the next-stage address is baked in at OpenSBI build
# time, so a0/a1 alone are enough.
OPENSBI ?= $(HOME)/Downloads/fw_jump.bin
RVVM    ?= /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64

all: firmware.bin

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Force the HAL to be (re)built with the S-mode flags. Phony so
# switching between examples doesn't ship a stale M-mode libhal.a.
.PHONY: hal-s
hal-s:
	$(MAKE) -C $(HAL) clean
	$(MAKE) -C $(HAL) HAL_PRIV=s HAL_PLAT=s_sbi HAL_OPT=$(HAL_OPT) HAL_LTO=$(HAL_LTO)

firmware.elf: $(OBJS) hal-s
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@
	@printf '\nBuilt %s (%s bytes)\n' "$@" "$$(stat -c %s $@)"

run: firmware.bin
	$(RVVM) $(OPENSBI) -k firmware.bin -nogui -nonet -nosound

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
