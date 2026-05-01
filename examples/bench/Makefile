HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy
RVVM     ?= /home/sol/repos/RVVM/release.linux.x86_64/rvvm_x86_64

# HAL_OPT and HAL_LTO mirror the libhal.a knobs so the firmware ELF
# is built consistently with the archive. With HAL_LTO=1 the consumer
# must also pass -flto so the linker can inline across .o boundaries.
HAL_OPT ?= s
HAL_LTO ?= 1   # bench is a microbenchmark — LTO drops unused HAL surface (~43% smaller bin)

CFLAGS   := -O$(HAL_OPT) -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -I$(HAL)/include
ifeq ($(HAL_LTO),1)
CFLAGS   += -flto=full
LTOLDFLAGS := -flto=full
else
LTOLDFLAGS :=
endif

LDFLAGS  := -nostdlib -static -Wl,-T,$(HAL)/link.ld $(LTOLDFLAGS)

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
	$(RVVM) firmware.bin -nogui -nonet -nosound

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
