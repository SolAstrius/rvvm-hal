# rvvm-hal — bare-metal RISC-V drivers for RVVM's emulated devices.
#
# Builds libhal.a — a static archive linkable into any bare-metal
# RVVM firmware. Headers live in include/, sources in src/. Consumers
# pass -Iinclude to find the headers and link libhal.a.

TARGET   := riscv64-freestanding-none
CC       := zig cc -target $(TARGET)
AR       := llvm-ar

CFLAGS   := -ffreestanding -fno-stack-protector -fno-pie \
            -mcmodel=medany -nostdlib \
            -Wall -Wextra -Wno-unused-parameter \
            -ffunction-sections -fdata-sections \
            -Iinclude

# Target ISA extensions beyond rv64gc. Selected to match what RVVM
# advertises in its riscv_exts string (src/rvvm.c) — running on RVVM
# we never trap on these. On real silicon, consumers should override
# HAL_MARCH_EXTS to match their target chip.
#
# Why these:
#   zba zbb zbs   Bitmanip — compiler emits sh*add, ctz, andn, min/max,
#                 sext.b/h. Pervasive small wins.
#   zicond        czero.{eqz,nez} for branch-free select.
#   zicclsm       Hardware-supported misaligned scalar L/S — lets the
#                 compiler emit unaligned loads/stores without the
#                 byte-fallback path.
#   zicboz        cbo.zero — 64-byte cache-block zero in one insn;
#                 useful if/when memset gets a Zicboz fast path.
#
# Opt-in extensions (add via `make HAL_MARCH_EXTS_EXTRA="zcb …"`):
#   zcb           Compressed byte/halfword load/store + zext/sext/mul/not
#                 in 16-bit encodings. Noticeable size win for MMIO-heavy
#                 drivers (UART, CLINT, PLIC byte/halfword pokes), but
#                 not in QEMU's default `rv64` cpu — keeping it off by
#                 default lets the same firmware.elf boot on QEMU virt
#                 without `-cpu` flags. RVVM enables it unconditionally,
#                 so opting in costs nothing on RVVM.
#
# zig cc routes -march= to -target-cpu, so we use clang's
# -target-feature mechanism via -Xclang to add each extension.
HAL_MARCH_EXTS ?= zba zbb zbs zicond zicclsm zicboz
HAL_MARCH_EXTS += $(HAL_MARCH_EXTS_EXTRA)
CFLAGS   += $(foreach ext,$(HAL_MARCH_EXTS),-Xclang -target-feature -Xclang +$(ext))

# Extra defines/flags from the consumer, e.g. tuning compile-time ceilings:
#   make HAL_EXTRA_CFLAGS=-DVIRTIO_GPU_VRAM_BYTES=0x100000
CFLAGS   += $(HAL_EXTRA_CFLAGS)

# Privilege mode + platform backend.
#
#   HAL_PRIV  ∈ {m, s}     — selects mstatus/mie/... vs sstatus/sie/...
#                            CSR aliases in include/priv.h. Default: m.
#   HAL_PLAT  ∈ {m_clint,  — picks src/plat_<plat>.c, the file
#                s_sbi,      implementing IPI / timer / intc for the
#                m_clic,     target machine.
#                s_aia}     Default: m_clint (M-mode + CLINT + PLIC),
#                            matching the stock RVVM machine.
#
# Today only m_clint is implemented; the other names are reserved for
# future backends (see include/plat.h header comment for the design).
HAL_PRIV ?= m
HAL_PLAT ?= m_clint

ifeq ($(HAL_PRIV),m)
CFLAGS   += -DHAL_PRIV_M=1
else ifeq ($(HAL_PRIV),s)
CFLAGS   += -DHAL_PRIV_S=1
else
$(error HAL_PRIV must be 'm' or 's' (got '$(HAL_PRIV)'))
endif

# Sanity-check: not every (priv, plat) pair makes sense. m_clint needs
# M-mode; s_sbi needs S-mode; etc. Reject obviously-wrong combos early
# so the failure isn't a confusing CSR-illegal trap at runtime.
ifeq ($(HAL_PLAT),m_clint)
ifneq ($(HAL_PRIV),m)
$(error HAL_PLAT=m_clint requires HAL_PRIV=m)
endif
else ifeq ($(HAL_PLAT),s_sbi)
ifneq ($(HAL_PRIV),s)
$(error HAL_PLAT=s_sbi requires HAL_PRIV=s)
endif
else ifeq ($(HAL_PLAT),m_clic)
ifneq ($(HAL_PRIV),m)
$(error HAL_PLAT=m_clic requires HAL_PRIV=m)
endif
else ifeq ($(HAL_PLAT),s_aia)
ifneq ($(HAL_PRIV),s)
$(error HAL_PLAT=s_aia requires HAL_PRIV=s)
endif
else
$(error HAL_PLAT='$(HAL_PLAT)' is not a known backend)
endif

# DEBUG=1 swaps the optimisation level and enables full debug info
# + frame pointers + the HAL_DEBUG macro (which lights up HAL_ASSERT).
# Frame pointers are required for the panic dumper's stack walk
# (src/panic.c) to produce more than two frames; without them the
# walk falls back to printing just mepc + ra. CI debug builds and
# `make DEBUG=1` for local gdb-stub work both flip this. Consumer
# firmwares should mirror this in their own Makefiles to keep the
# layout consistent — picolibc headers don't depend on HAL_DEBUG but
# panic.h's HAL_ASSERT macro does.
ifeq ($(DEBUG),1)
# -fsanitize=undefined enables UBSan; -fsanitize-trap=undefined turns
# its runtime calls into ud2-style traps instead of __ubsan_handle_*
# library calls (which would need libubsan we don't ship in
# freestanding). The resulting traps land in __trap_entry as
# "illegal instruction" and pretty-print through the panic dumper —
# so UB at runtime gives you a backtrace to the offending line.
# zig cc enables UBSan implicitly at -Og without these flags, which
# is why we set them explicitly rather than -fno-sanitize=all.
CFLAGS   += -Og -g3 -gdwarf-4 -fno-omit-frame-pointer -DHAL_DEBUG \
            -fsanitize=undefined -fsanitize-trap=undefined
else
# Optimisation level — `s` (size) is the default; `2` is a speed-leaning
# setting that unrolls loops more aggressively and inlines wider, at
# some binary-size cost. Override with `make HAL_OPT=2` for a perf-
# leaning build of libhal.a.
HAL_OPT  ?= s
CFLAGS   += -O$(HAL_OPT) -fno-sanitize=all
endif

# Optional: HAL_LTO=1 turns on LLVM thin/full LTO across the libhal.a
# build. Object files become LLVM bitcode (llvm-ar handles those);
# consumer firmwares that also pass -flto at link time will then
# inline HAL functions across translation-unit boundaries (mmio
# accessors, plat_* operations, atomic primitives, fdt walkers).
# Cost: link time ~3-5×, build flow unchanged otherwise.
ifeq ($(HAL_LTO),1)
CFLAGS   += -flto=full
endif

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

# Optional: HAL_NO_SSTC=1 forces the S+SBI backend off the Sstc
# fast-path even when the FDT advertises it. Used for A/B comparison
# of the direct-CSR vs sbi_set_timer ecall paths; not for normal use.
ifeq ($(HAL_NO_SSTC),1)
CFLAGS   += -DHAL_NO_SSTC
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

# Optional: HAL_LWIP=1 includes vendored lwIP 2.2.1 for TCP/IP. Adds
# ~150 KiB to libhal.a, but per-firmware cost is whatever the
# consumer actually references (DHCP-only ≈ 25 KiB; full TCP/UDP
# server ≈ 60 KiB). Requires HAL_PICOLIBC (printf for diagnostics,
# rand for TCP ISN).
ifeq ($(HAL_LWIP),1)
ifeq ($(HAL_PICOLIBC),)
$(error HAL_LWIP=1 requires HAL_PICOLIBC=min or std)
endif
CFLAGS   += -DHAL_LWIP \
            -Ivendor/lwip-config \
            -Ivendor/lwip/src/include
endif

SRCS     := $(wildcard src/*.c) $(wildcard src/*.S)

# Strip every plat_*.c, then add back only the one the build selected.
# Keeps `make HAL_PLAT=...` deterministic regardless of which backend
# files happen to exist on disk (unselected ones may be in-progress
# stubs that don't compile yet).
SRCS     := $(filter-out $(wildcard src/plat_*.c),$(SRCS))
SRCS     += src/plat_$(HAL_PLAT).c

# The boot path differs between privilege modes — start.S takes the
# RVVM-bare-metal reset-PC entry; start_s.S takes the SBI handoff ABI.
# Build the right one for HAL_PRIV.
ifeq ($(HAL_PRIV),m)
SRCS     := $(filter-out src/start_s.S,$(SRCS))
else
SRCS     := $(filter-out src/start.S,$(SRCS))
endif

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

# string.S is the asm-unrolled memcpy/memset/memmove. Default-on; opt
# out with HAL_NO_ASM_STRING=1 (falls back to the C versions in
# string.c). The asm versions are ~2× faster than the C ones on the
# aligned hot path, written in pure RV64I so they compile under any
# extension subset. When picolibc is on, *both* are excluded since
# picolibc supplies its own optimised mem*/str* family.
ifeq ($(HAL_NO_ASM_STRING),1)
SRCS     := $(filter-out src/string_asm.S,$(SRCS))
else
ifeq ($(HAL_PICOLIBC),)
# asm version active: drop the C string.c so symbols don't collide
SRCS     := $(filter-out src/string.c,$(SRCS))
else
# picolibc on → string.c already excluded above; also exclude .S
SRCS     := $(filter-out src/string_asm.S,$(SRCS))
endif
endif
ifneq ($(HAL_FATFS),1)
# Without HAL_FATFS, fatfs_disk.c is a no-op (#ifdef gates everything).
SRCS     := $(filter-out src/fatfs_disk.c,$(SRCS))
endif
ifneq ($(HAL_LWIP),1)
# Without HAL_LWIP, net.c is a no-op (#ifdef gates everything).
SRCS     := $(filter-out src/net.c,$(SRCS))
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

# When HAL_LWIP is on, compile lwIP's core, ipv4, and netif/ethernet.
# Skipped: api/ (sequential socket layer needs threads), ipv6/, the
# specialty netif drivers (ppp, slipif, lowpan6, bridge), apps/.
ifeq ($(HAL_LWIP),1)
LWIP_SRCS := \
    vendor/lwip/src/core/init.c \
    vendor/lwip/src/core/def.c \
    vendor/lwip/src/core/dns.c \
    vendor/lwip/src/core/inet_chksum.c \
    vendor/lwip/src/core/ip.c \
    vendor/lwip/src/core/mem.c \
    vendor/lwip/src/core/memp.c \
    vendor/lwip/src/core/netif.c \
    vendor/lwip/src/core/pbuf.c \
    vendor/lwip/src/core/raw.c \
    vendor/lwip/src/core/stats.c \
    vendor/lwip/src/core/sys.c \
    vendor/lwip/src/core/tcp.c \
    vendor/lwip/src/core/tcp_in.c \
    vendor/lwip/src/core/tcp_out.c \
    vendor/lwip/src/core/timeouts.c \
    vendor/lwip/src/core/udp.c \
    vendor/lwip/src/core/altcp.c \
    vendor/lwip/src/core/altcp_alloc.c \
    vendor/lwip/src/core/altcp_tcp.c \
    vendor/lwip/src/core/ipv4/dhcp.c \
    vendor/lwip/src/core/ipv4/etharp.c \
    vendor/lwip/src/core/ipv4/icmp.c \
    vendor/lwip/src/core/ipv4/ip4.c \
    vendor/lwip/src/core/ipv4/ip4_addr.c \
    vendor/lwip/src/core/ipv4/ip4_frag.c \
    vendor/lwip/src/core/ipv4/acd.c \
    vendor/lwip/src/netif/ethernet.c
OBJS     += $(patsubst vendor/lwip/src/%.c,build/lwip/%.o,$(LWIP_SRCS))
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

# Vendored lwIP sources. The core/ tree compiles cleanly under our
# CFLAGS; ipv4/ and netif/ live in subdirs so we mkdir build/lwip/...
# accordingly.
build/lwip/%.o: vendor/lwip/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-address-of-packed-member \
	    -Wno-unused-but-set-variable \
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

# ---------------------------------------------------------------------
# QEMU virt convenience targets.
#
# rvvm-hal is FDT-driven, and QEMU's `virt` machine happens to share
# every magic address we care about with RVVM's default machine
# (NS16550A at 0x10000000, CLINT at 0x02000000, PLIC at 0x0c000000,
# PCI ECAM at 0x30000000, syscon at 0x00100000, goldfish-rtc at
# 0x00101000). The same firmware binary runs on both — only the host
# launcher differs.
#
# QEMU_CPU enables the same RISC-V extensions libhal.a is compiled
# with (HAL_MARCH_EXTS). `-cpu max` would also work; we list explicit
# knobs so an extension we accidentally lean on without listing it in
# HAL_MARCH_EXTS shows up as a clean illegal-instruction trap rather
# than being silently available. zicclsm isn't a CPU knob in QEMU
# (misaligned access is on by default for `virt`), so it's omitted.
# Add `zcb=true` here if you also pass HAL_MARCH_EXTS_EXTRA=zcb.
QEMU       ?= qemu-system-riscv64
QEMU_CPU   ?= rv64,zba=true,zbb=true,zbs=true,zicond=true,zicboz=true
# `force-legacy=false`: by default QEMU's virtio-mmio bus presents
# devices as legacy virtio (Version=1), which our driver doesn't
# implement. Forcing modern (Version=2) on every slot lets the same
# binary use any -device virtio-*-device the user attaches.
QEMU_FLAGS ?= -M virt -nographic -cpu $(QEMU_CPU) \
              -global virtio-mmio.force-legacy=false

run-qemu-probe:
	$(MAKE) -C examples/probe firmware.elf
	$(QEMU) $(QEMU_FLAGS) -bios none -kernel examples/probe/firmware.elf

run-qemu-probe-s:
	$(MAKE) -C examples/probe-s firmware.elf
	$(QEMU) $(QEMU_FLAGS) -bios default -kernel examples/probe-s/firmware.elf

# Deterministic bench: QEMU `-icount` retires one mtime tick per
# instruction, so wall-clock noise drops out and runs are bit-identical.
# Real performance still requires real hardware, but this is the right
# tool for code-gen A/B testing (Zcb-vs-no-Zcb, asm-vs-C string ops…).
run-qemu-bench:
	$(MAKE) -C examples/bench run-qemu-icount

.PHONY: all clean run-audio-beep run-audio-edge run-audio-pcm run-probe run-smp \
        run-qemu-probe run-qemu-probe-s run-qemu-bench \
        picolibc-min picolibc-std picolibc-clean
