# rvvm-hal

A bare-metal Hardware Abstraction Layer for [RVVM](https://github.com/LekKit/RVVM)
— the lightweight RISC-V emulator. Target audience: people writing
M-mode firmware that runs directly on RVVM with no SBI / no kernel,
who want to ship anything from a 7 KB hello-world to a full unikernel
(libc + filesystem + TCP/IP + SMP) without rolling each piece by hand.

The base HAL is ~2K lines of C exposing every device RVVM emulates.
Optional layers add picolibc, FatFs, and lwIP; each is opt-in via a
build flag and contributes zero bytes to firmwares that don't use it
(`-Wl,--gc-sections` trims aggressively).

## Drivers — always available

| device | provides | RVVM source |
|---|---|---|
| `uart`   | NS16550A — `printf`, `getc`, hex dump | `src/devices/ns16550a.c` |
| `fdt`    | Flattened Device Tree walker — `compatible` lookup, `reg` decode | (consumed via `a1` at boot) |
| `pci`    | ECAM scanner, BAR readback, capability list, MSI configure | `src/devices/pci-bus.c` |
| `irq`    | SiFive PLIC + RISC-V trap dispatch | `src/devices/riscv-plic.c` |
| `smp`    | multi-hart detection (FDT `/cpus`), CLINT MSIP wakeup, `smp_start(hartid, fn, arg)` | `src/devices/riscv-aclint.c` |
| `time`   | `rdtime` CSR + CLINT mtimecmp idle-wait via `wfi` | `src/devices/riscv-aclint.c` |
| `rtc`    | Google Goldfish RTC — `rtc_now_seconds()`, wallclock | `src/devices/rtc-goldfish.c` |
| `atomic` | RV-A wrappers (`amoadd`/`lr`/`sc` typed inlines), `mutex_t` spinlock | — |
| `bochs`/`gfx`/`gfx_text` | Bochs Display + simple-framebuffer auto-select, char-grid renderer | `src/devices/bochs-display.c` |
| `i2c`    | OpenCores I²C master — write / write-then-read, polling | `src/devices/i2c-oc.c` |
| `hid`    | HID-over-I²C boot keyboard, key-diff event emit | `src/devices/i2c-hid.c` |
| `nvme`   | NVMe-over-PCIe block device, chained PRP, large transfers | `src/devices/nvme.c` |
| `audio`/`hda` | Intel HDA controller — beep widget + raw 16-bit PCM streaming, ALSA-period-aligned BDL | `src/devices/sound-hda.c` |
| `eth`    | Realtek RTL8169 — descriptor-mode RX/TX, raw L2 frames | `src/devices/rtl8169.c` |
| `mmio`/`string` | volatile MMIO accessors, word-aligned mem*` helpers | — |

Plus `rvvm.h` — the topology header. Documents every magic address,
PCI device ID, and register offset RVVM uses, with cross-refs back
into RVVM's source.

## Optional layers — opt-in via `HAL_X=1`

| flag | adds | size in firmware |
|---|---|---|
| `HAL_PICOLIBC=min` | picolibc 1.8.11 — `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<ctype.h>`, integer printf | ~3-10 KiB depending on use |
| `HAL_PICOLIBC=std` | + float/double printf, `<math.h>`, 64-bit `%lld` | +20-40 KiB depending on use |
| `HAL_FATFS=1` | FatFs r0.15a — FAT12/16/32/exFAT RW on top of NVMe | ~11 KiB for typical f_open/read/write |
| `HAL_LWIP=1` | lwIP 2.2.1 — DHCP, ARP, ICMP, IP, TCP, UDP, DNS | ~50-150 KiB depending on use |
| `HAL_NO_SMP=1` | strips multi-hart support (single-hart firmwares) | savings: ~3 KiB code + 64 KiB stack |

Each flag must be set on **both** the HAL build and your firmware's
own CFLAGS so prototypes match symbols. Without flags, the HAL stays
at its bare-metal baseline (~30 KiB compiled, ~7 KiB after gc).

## Build

Requires `zig` (used as `zig cc -target riscv64-freestanding-none`)
and `llvm-ar`. The flake provides both, plus `meson` + `ninja` for
picolibc:

```sh
nix develop --command make                   # baseline libhal.a
nix develop --command make HAL_PICOLIBC=min  # + libc
nix develop --command make HAL_PICOLIBC=min HAL_FATFS=1            # + FS
nix develop --command make HAL_PICOLIBC=min HAL_FATFS=1 HAL_LWIP=1 # full unikernel
```

First-time picolibc/lwIP builds also fetch the submodules:

```sh
git submodule update --init --recursive
make picolibc-min picolibc-std    # one-time, ~30s each
```

## Use from your own firmware

Add as a git submodule:

```sh
git submodule add https://github.com/SolAstrius/rvvm-hal vendor/rvvm-hal
git -C vendor/rvvm-hal submodule update --init --recursive
```

Minimal `Makefile`:

```make
HAL := vendor/rvvm-hal

CFLAGS  += -I$(HAL)/include
LDFLAGS += -Wl,-T,$(HAL)/link.ld -Wl,--gc-sections

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

firmware.elf: $(YOUR_OBJS) $(HAL)/libhal.a
	zig cc -target riscv64-freestanding-none -nostdlib -static \
	    $(LDFLAGS) -o $@ $(YOUR_OBJS) $(HAL)/libhal.a
```

Minimal `main.c`:

```c
#include "uart.h"

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("hello from RVVM bare metal!\n");
    for (;;) __asm__ volatile ("wfi");
}
```

`src/start.S` provides `_start` — parks secondary harts in `wfi`
on `mie.MSIE`, sets per-hart sp from `__stack_top`, zeroes BSS,
forwards `a0=hartid` + `a1=fdt_addr` into `kmain`. Per-hart 16 KiB
stacks reserved by `link.ld` (8 × 16 KiB = 128 KiB total).

## Run on RVVM

```sh
# Bare metal: UART only
rvvm firmware.bin -nogui -nonet -nosound

# With graphics + keyboard
rvvm firmware.bin -bochs_display

# With NVMe-backed disk image
rvvm firmware.bin -nvme mydisk.img

# Multi-core
rvvm firmware.bin -smp 4

# Networking (default; -nonet to disable)
rvvm firmware.bin -portfwd udp/2007=7   # forward host:2007 → guest:7
```

## Examples

| example | demonstrates |
|---|---|
| [`examples/probe/`](examples/probe/) | bare HAL surface — FDT walk, gfx auto-select, NVMe LBA0 dump, IRQ-driven UART RX |
| [`examples/audio-beep/`](examples/audio-beep/) | HDA codec beep widget — C-major scale via `audio_beep(hz)` |
| [`examples/audio-edge/`](examples/audio-edge/) | emulator-cycle-driven 1-bit speaker — Speccy/Apple II shape |
| [`examples/audio-pcm/`](examples/audio-pcm/) | raw 16-bit PCM streaming with continuous-feed pattern |
| [`examples/smp/`](examples/smp/) | multi-hart wake-up + atomic/mutex contention test (200K iters × 4 harts) |
| [`examples/picolibc-hello/`](examples/picolibc-hello/) | real `printf` / `malloc` / `strtol` / `qsort` |
| [`examples/fs-hello/`](examples/fs-hello/) | exFAT image: `f_open` / `f_read` / `f_write` / `f_size`, file persists across boots |
| [`examples/eth-hello/`](examples/eth-hello/) | RTL8169 raw L2: ARP request → reply, decode |
| [`examples/net-hello/`](examples/net-hello/) | full TCP/IP — DHCP client gets an IP, UDP echo server on port 7 |

## Real-world consumers

| consumer | uses |
|---|---|
| [**scev-chip-8**](https://github.com/SolAstrius/scev-chip-8) | bare HAL — UART, gfx, HID, NVMe |
| [**scev-cores/apple-1**](https://github.com/SolAstrius/scev-cores) | + picolibc-min |
| [**scev-cores/zx-spectrum**](https://github.com/SolAstrius/scev-cores) | bare HAL + custom snapshot loader |
| [**scev-cores/game-boy**](https://github.com/SolAstrius/scev-cores) | + picolibc-min, vendored binjgb |
| [**scev-cores/game-boy-advance**](https://github.com/SolAstrius/scev-cores) | + picolibc-min, vendored gdkGBA |
| [**scev-cores/basic**](https://github.com/SolAstrius/scev-cores) | bare HAL + custom shim, vendored bwbasic 3.20 |

Each pins this repo via submodule. Working references for every
opt-in flag combination.

## Versioning

Git-tagged following semver. Pin a specific version in your submodule
for stability:

```sh
git -C vendor/rvvm-hal checkout v1.0.0
git add vendor/rvvm-hal && git commit -m "pin rvvm-hal v1.0.0"
```

API contract (post-1.0): driver functions don't break across minor
versions. New optional layers and additions go to minor bumps;
breaking changes go to major bumps.

## License

MIT-ish for the HAL itself. Treat as public-domain reference code.

Vendored libraries live under `vendor/` with their own licenses
(all permissive — picolibc is BSD-1c, FatFs is BSD-1c, lwIP is
BSD-3c). The RVVM-side device emulators it talks to are in
`LekKit/RVVM` under MPL-2.0 — the address constants and register
layouts in `include/rvvm.h` were derived from reading that source.
RVVM itself isn't redistributed here.
