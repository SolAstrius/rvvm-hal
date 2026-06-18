# rmm-test — RVVM issue #204 RMM (roundTiesToAway) regression harness.
HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

RVVM     ?= $(shell command -v rvvm 2>/dev/null)

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

build/%.o: %.c vectors.inc
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: $(HAL)/libhal.a
$(HAL)/libhal.a:
	$(MAKE) -C $(HAL) HAL_OPT=$(HAL_OPT) HAL_LTO=$(HAL_LTO)

firmware.elf: $(OBJS) $(HAL)/libhal.a
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(HAL)/libhal.a

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@

run: firmware.bin
	$(RVVM) firmware.bin -nogui -nonet -nosound

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
