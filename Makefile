# rvvm-hal — bare-metal RISC-V drivers for RVVM's emulated devices.
#
# Builds libhal.a — a static archive linkable into any bare-metal
# RVVM firmware. Headers live in include/, sources in src/. Consumers
# pass -Iinclude to find the headers and link libhal.a.

TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
AR       := llvm-ar

CFLAGS   := -Os -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -Iinclude

SRCS     := $(wildcard src/*.c) $(wildcard src/*.S)
OBJS     := $(patsubst src/%.c,build/%.o,$(filter %.c,$(SRCS))) \
            $(patsubst src/%.S,build/%.o,$(filter %.S,$(SRCS)))

all: libhal.a

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# string.c implements memcpy/memset/memmove. The compiler can recognise
# these bodies and rewrite them into self-calls; -fno-builtin (plus the
# per-function no_builtin attribute in the source) suppresses that.
build/string.o: src/string.c
	@mkdir -p build
	$(CC) $(CFLAGS) -fno-builtin -c -o $@ $<

build/%.o: src/%.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

libhal.a: $(OBJS)
	$(AR) rcs $@ $^
	@printf '\nBuilt %s (%s bytes, %d objects)\n' \
		"$@" "$$(stat -c %s $@)" "$$(echo $^ | wc -w)"

clean:
	rm -rf build libhal.a

# ---------------------------------------------------------------------
# Convenience: build & run examples from the HAL root.
#
#   make run-audio-beep   — codec beep widget; plays C major scale.
#                           Smallest possible audio path: links only
#                           audio.o + hda.o (~16 KB ELF).
#   make run-audio-edge   — emulator-cycle-driven 1-bit speaker;
#                           sweeps 1 kHz / 440 Hz / 220 Hz / silence
#                           against a synthetic 3.5 MHz Z80-like clock.
#   make run-audio-pcm    — raw PCM streaming; alternates 480 Hz / 240
#                           Hz / silence with continuous-feed pattern.
#
# All require RVVM started with -hda_test (we pass that automatically).
# Defaults to `rvvm` on PATH; override with `make run-audio-beep RVVM=…`.

run-audio-beep:
	$(MAKE) -C examples/audio-beep run

run-audio-edge:
	$(MAKE) -C examples/audio-edge run

run-audio-pcm:
	$(MAKE) -C examples/audio-pcm run

run-probe:
	$(MAKE) -C examples/probe run

.PHONY: all clean run-audio-beep run-audio-edge run-audio-pcm run-probe
