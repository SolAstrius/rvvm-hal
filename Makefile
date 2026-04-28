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
            -ffunction-sections -fdata-sections \
            -Iinclude

# -ffunction-sections + -fdata-sections puts every function and
# global into its own ELF section. Combined with `-Wl,--gc-sections`
# at consumer firmware link time, this lets unreferenced HAL symbols
# (FatFs internal helpers, unused IRQ counters, audio paths the
# consumer never touches) drop out of the final binary entirely.

# Optional: HAL_NO_SMP=1 strips multi-hart support. start.S parks every
# non-zero hart with no per-hart stack math, smp.c isn't compiled, and
# smp.h becomes a stub returning hart_count=1 / smp_start=false. Useful
# for: (a) firmwares targetting tiny FPGA softcores where the 128 KiB
# stack reservation matters, (b) consumers that want the smallest
# possible boot path. Cost otherwise is ~3 KiB of code + 64 KiB of
# unused stack address space — both negligible on RVVM (256 MiB RAM
# default), but the option exists for the cases where it isn't.
#
# Consumer firmwares set this in their own Makefile:
#   $(MAKE) -C $(HAL) HAL_NO_SMP=1
ifeq ($(HAL_NO_SMP),1)
CFLAGS   += -DHAL_NO_SMP
endif

# Optional: HAL_PICOLIBC=min|std links picolibc into firmwares that
# include this build of libhal.a. Two variants live under
# vendor/picolibc-build/{min,std}/install — built on demand by the
# `picolibc-min` / `picolibc-std` targets below. See README's libc
# section for the per-variant feature matrix.
#
# When HAL_PICOLIBC is on:
#   - src/picolibc_hooks.c is compiled in (FILE put/get → uart, _sbrk)
#   - src/string.c is excluded (picolibc has its own mem* + better
#     str* family; ours would shadow it).
#   - Consumer firmwares must add the picolibc headers + libs to
#     their own CFLAGS/LDFLAGS — see examples/picolibc-hello/Makefile.
ifneq ($(HAL_PICOLIBC),)
CFLAGS   += -DHAL_PICOLIBC \
            -isystem vendor/picolibc-build/$(HAL_PICOLIBC)/install/include
endif

# Optional: HAL_FATFS=1 includes vendored FatFs (vendor/fatfs/) for
# FAT/exFAT-on-NVMe support. Adds ~22 KiB to libhal.a — every byte
# strippable via -Wl,--gc-sections at firmware link time if a
# consumer doesn't actually call f_open / f_mkfs / f_chdir / etc.
# Requires HAL_PICOLIBC (FatFs needs malloc + string functions).
ifeq ($(HAL_FATFS),1)
ifeq ($(HAL_PICOLIBC),)
$(error HAL_FATFS=1 requires HAL_PICOLIBC=min or std)
endif
CFLAGS   += -DHAL_FATFS -Ivendor/fatfs
endif

SRCS     := $(wildcard src/*.c) $(wildcard src/*.S)
ifeq ($(HAL_NO_SMP),1)
SRCS     := $(filter-out src/smp.c,$(SRCS))
endif
ifeq ($(HAL_PICOLIBC),)
# Without picolibc, picolibc_hooks.c is a no-op (#ifdef gates everything)
# but excluding it from the archive avoids a dead .o entirely.
SRCS     := $(filter-out src/picolibc_hooks.c,$(SRCS))
else
SRCS     := $(filter-out src/string.c,$(SRCS))
endif
ifneq ($(HAL_FATFS),1)
# Without HAL_FATFS, fatfs_disk.c is a no-op (#ifdef gates everything).
SRCS     := $(filter-out src/fatfs_disk.c,$(SRCS))
endif

OBJS     := $(patsubst src/%.c,build/%.o,$(filter %.c,$(SRCS))) \
            $(patsubst src/%.S,build/%.o,$(filter %.S,$(SRCS)))

# When HAL_FATFS is on, also compile vendor/fatfs/ff.c and
# vendor/fatfs/ffunicode.c into libhal.a. Per-function-sections still
# applies, so a consumer that only uses f_open + f_read + f_close
# pays ~10 KiB; a consumer that never calls FatFs at all gc-sections
# the lot.
ifeq ($(HAL_FATFS),1)
OBJS     += build/fatfs/ff.o build/fatfs/ffunicode.o
endif

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

# Vendored FatFs sources, compiled with the same CFLAGS plus a few
# warning suppressions for upstream code we don't want to patch.
build/fatfs/%.o: vendor/fatfs/%.c
	@mkdir -p build/fatfs
	$(CC) $(CFLAGS) -Wno-unused-but-set-variable -Wno-format \
	    -c -o $@ $<

libhal.a: $(OBJS)
	$(AR) rcs $@ $^
	@printf '\nBuilt %s (%s bytes, %d objects)\n' \
		"$@" "$$(stat -c %s $@)" "$$(echo $^ | wc -w)"

clean:
	rm -rf build libhal.a

# ---------------------------------------------------------------------
# Vendored picolibc — built once, cached under vendor/picolibc-build/.
#
#   picolibc-min   integer printf only, no semihost, no posix-io,
#                  no locale tables. ~30 KiB total before gc-sections;
#                  pulls a few hundred bytes into a chip-8/apple-1
#                  style firmware.
#   picolibc-std   adds float/double printf and the math library.
#                  For game-boy-advance (libm sqrt/sin/cos paths)
#                  and bwbasic (full stdio + qsort + math).
#
# Both produce <variant>/install/{lib/libc.a, include/} ready to feed
# into a consumer's -isystem / -L flags. Re-run `make picolibc-clean`
# to start fresh; otherwise the targets are idempotent.

PICOLIBC_DIR := vendor/picolibc

# Common meson flags — disable everything we definitely don't need
# regardless of variant. Float/printf format is the variant axis.
PICOLIBC_COMMON := \
    --cross-file=../../picolibc-cross.txt \
    --buildtype=minsize \
    -Dposix-console=false \
    -Dsemihost=false \
    -Dpicocrt=false \
    -Dtests=false \
    -Dmultilib=false \
    -Dincludedir=include \
    -Dlibdir=lib

picolibc-min: vendor/picolibc-build/min/install/lib/libc.a
picolibc-std: vendor/picolibc-build/std/install/lib/libc.a

vendor/picolibc-build/min/install/lib/libc.a:
	@mkdir -p vendor/picolibc-build/min
	cd vendor/picolibc-build/min && meson setup $(PICOLIBC_COMMON) \
	    --prefix=$$(pwd)/install \
	    -Dformat-default=integer \
	    ../../picolibc
	cd vendor/picolibc-build/min && meson compile && meson install
	@printf '\nBuilt picolibc-min: %s\n' "$$(stat -c %s $@) bytes (libc.a)"

vendor/picolibc-build/std/install/lib/libc.a:
	@mkdir -p vendor/picolibc-build/std
	cd vendor/picolibc-build/std && meson setup $(PICOLIBC_COMMON) \
	    --prefix=$$(pwd)/install \
	    -Dformat-default=double \
	    -Dio-long-long=true \
	    ../../picolibc
	cd vendor/picolibc-build/std && meson compile && meson install
	@printf '\nBuilt picolibc-std: %s\n' "$$(stat -c %s $@) bytes (libc.a)"

picolibc-clean:
	rm -rf vendor/picolibc-build

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

run-smp:
	$(MAKE) -C examples/smp run

.PHONY: all clean run-audio-beep run-audio-edge run-audio-pcm run-probe run-smp \
        picolibc-min picolibc-std picolibc-clean
