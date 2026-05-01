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
| `ui`     | menu / confirm / message / file-picker primitives, dual UART + GFX backend | — |
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
| [`examples/ui-hello/`](examples/ui-hello/) | menu primitives — top-level menu, file picker, yes/no dialog, message banner |

## Debugging

The HAL ships a panic / assert / clean-exit primitive set built around
a single structured UART format. Three call sites that matter:

| call | when |
|---|---|
| `hal_panic("msg %d", x)` | unrecoverable error; capture call site, dump regs + backtrace, hang |
| `hal_exit(code)`         | clean shutdown; logs `!!HAL-EXIT code=N`, writes SYSCON_POWEROFF, RVVM exits |
| `HAL_CHECK(cond)`        | always-live invariant; failure routes to `hal_panic` |
| `HAL_ASSERT(cond)`       | debug-only invariant; compiles to `((void)0)` without `-DHAL_DEBUG` |

Hardware traps that aren't handled (load-fault, illegal instruction,
ecall from M, page-fault, …) reach the same dumper via the trap path
in `src/irq.c`, so any bug — your code or the HAL's — produces an
identical, parseable frame.

### The panic format

Every panic emits a stable `!!HAL-PANIC-V1` block on UART:

```
!!HAL-PANIC-V1 ============================================
   cause      : load access fault (mcause=0x05)
   mepc       : 0x0000000080003a4c
   mtval      : 0x00000000deadbeef
   mstatus    : 0x0000000000001880
   mtvec      : 0x0000000080012080
   mhartid    : 0
   ----
   x0   zero  : 0x0000000000000000
   x1   ra    : 0x0000000080004f10
   ...
   x31  t6    : 0x0000000000000000
   ----
   stack:
     #0  0x0000000080003a4c
     #1  0x0000000080004f10
     #2  0x0000000080008a04
   ...
!!HAL-PANIC-V1 end ========================================
```

The format is versioned (V1) so future changes can be tooling-aware.
Re-entrant panics (a fault inside the dumper itself, or a parallel
panic on another hart) short-circuit to `wfi` so the on-wire dump
stays readable.

### Symbolicating the dump

```sh
rvvm firmware.bin -nogui 2>&1 | tee /tmp/uart.log
./tools/decode-panic firmware.elf < /tmp/uart.log
```

`decode-panic` rewrites every `0x80…` literal through `addr2line`,
yielding `function at file:line` annotations next to each PC, register
value, and backtrace frame. Picks `llvm-addr2line` first, then any
`riscv64-*-addr2line`, then plain `addr2line` (which only works if
your host binutils was built multi-arch). Override with
`RVVM_ADDR2LINE=/path/to/addr2line`.

For the backtrace to extend past `mepc + ra`, build the HAL **and**
your firmware with `make DEBUG=1` (frame pointers are required for
the fp-walk).

### `make DEBUG=1`

Swaps `-Os` for `-Og -g3 -gdwarf-4 -fno-omit-frame-pointer`, adds
`-DHAL_DEBUG` (lights up `HAL_ASSERT`), and turns on trap-mode UBSan
(`-fsanitize=undefined -fsanitize-trap=undefined`). UB at runtime
becomes an "illegal instruction" trap that prints through the panic
dumper, so signed overflow / shift-out-of-range / null-deref get
caught with a backtrace to the offending line.

Mirror the flag in your firmware's own Makefile so prototype
visibility (the `HAL_ASSERT` macro in particular) matches the HAL
build.

### Live debugging via gdb

RVVM has a built-in gdbstub. Launch the VM paused with the gdbstub
flag (see RVVM's own `-h`), then attach any RV64-aware gdb:

```sh
gdb-multiarch firmware.elf
(gdb) target remote :1234
(gdb) break kmain
(gdb) continue
```

Source-level breakpoints, register inspection, and step are all
available. Loading the ELF gives gdb the symbols + DWARF that the
panic dumper otherwise needs `decode-panic` to resolve.

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
