# audio-pcm — minimal HDA PCM streaming demo. Same toolchain shape as
# examples/probe/. Fires up the HDA controller, allocates a 4800-frame
# (100 ms) ring, fills it with audio, and lets RVVM's stream worker
# loop the ring forever.

HAL      := ../..
TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
OBJCOPY  := llvm-objcopy

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

# Convenience: the user's RVVM build expects -hda_test to attach the
# device. Without that flag the firmware will print "device not found"
# and spin idle.
run: firmware.bin
	rvvm firmware.bin -nogui -nonet -hda_test

clean:
	rm -rf build firmware.elf firmware.bin

.PHONY: all run clean
