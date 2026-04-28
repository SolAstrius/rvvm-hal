# rvvm-hal

A small bare-metal Hardware Abstraction Layer for [RVVM](https://github.com/LekKit/RVVM)
— the lightweight RISC-V emulator. Target audience: people writing
M-mode firmware that runs directly on RVVM with no SBI / no kernel,
and wants a working device stack in ~1500 lines of C.

Drivers included:

| device | provides | RVVM source it talks to |
|---|---|---|
| `uart`  | NS16550A — `printf`, `getc`, hex dump | `src/devices/ns16550a.c` |
| `fdt`   | Flattened Device Tree walker — `compatible` lookup, `reg` decode | (consumed via `a1` at boot) |
| `pci`   | ECAM scanner, BAR readback (no sizing — RVVM pre-assigns) | `src/devices/pci-bus.c` |
| `bochs` | Bochs Display PCI 1234:1111 — mode-set, linear XRGB8888 framebuffer | `src/devices/bochs-display.c` |
| `i2c`   | OpenCores I²C master — write / write-then-read, polling | `src/devices/i2c-oc.c` |
| `hid`   | HID-over-I²C boot keyboard, key-diff event emit | `src/devices/i2c-hid.c` + `hid-keyboard.c` |
| `ata`   | PCI ATA PIO read, single-sector loop | `src/devices/ata.c` |
| `smp`   | multi-hart detection (FDT `/cpus`), CLINT MSIP wakeup, opt-in `smp_start(hartid, fn, arg)` | `src/devices/riscv-aclint.c` |
| `mmio`/`time`/`string` | utility helpers (volatile MMIO, mtime CSR, memset/cpy/move/cmp) | — |

Plus `rvvm.h` — the topology header. Documents every magic address,
PCI device ID, and register offset RVVM uses, with cross-refs back
into the RVVM source. Intended as fallback constants once FDT
discovery has populated the real values.

## Build

Requires `zig` (>= 0.13, used as `zig cc -target riscv64-freestanding-none`)
and `llvm-ar`. The flake provides both:

```sh
nix develop --command make
# → libhal.a (~30 KB static archive)
```

## Use from your own firmware

Add as a git submodule:

```sh
git submodule add https://github.com/SolAstrius/rvvm-hal vendor/rvvm-hal
```

Your `Makefile`:

```make
HAL := vendor/rvvm-hal

CFLAGS  += -I$(HAL)/include
LDFLAGS += -T$(HAL)/link.ld

$(HAL)/libhal.a:
	$(MAKE) -C $(HAL)

firmware.elf: $(YOUR_OBJS) $(HAL)/libhal.a
	zig cc -target riscv64-freestanding-none -nostdlib -static \
	    $(LDFLAGS) -o $@ $(YOUR_OBJS) $(HAL)/libhal.a
```

Your `main.c`:

```c
#include "uart.h"
#include "fdt.h"
#include "bochs.h"

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);                      // 0 = use rvvm.h fallback
    uart_puts("hello from RVVM bare metal!\n");
    // ... see examples/probe/main.c for a full HAL walkthrough
}
```

You also need a `_start` — `src/start.S` is provided. It parks
secondary harts, zeroes BSS, sets `sp` from `__stack_top` (defined
in `link.ld`), and forwards `a0=hartid` + `a1=fdt_addr` into
`kmain`.

## Run on RVVM

```sh
# Headless (UART only)
rvvm firmware.bin -nogui -nonet -nosound

# With graphical framebuffer + HID keyboard
rvvm firmware.bin -bochs_display -nonet -nosound

# Mount a disk image as ATA
rvvm firmware.bin -bochs_display -ata mydisk.bin
```

## Example

[`examples/probe/`](examples/probe/) — boots, walks the FDT, lists
every device found, brings up bochs (when available) with a colour
test pattern, dumps ATA disk info if mounted, echoes HID keystrokes
to the UART. Demonstrates the full HAL surface in a single ~150
line `main.c`.

```sh
cd examples/probe
nix develop ../.. --command make
rvvm firmware.bin -bochs_display
```

## Multi-hart

By default `_start` parks every hart with `mhartid != 0` in a tight
`wfi` loop on `mie.MSIE`, with a fresh per-hart 16 KiB stack. Single-
hart firmwares ignore SMP entirely — secondaries stay parked, primary
runs as before.

Consumers that want parallelism include `smp.h` and call:

```c
smp_init(&fdt);                          // count /cpus children
uart_printf("%u harts\n", smp_hart_count());

smp_start(1, run_emulator_step, &state); // wake hart 1 with work
do_other_work_on_primary();
smp_wait(1);                             // join
```

The wake mechanism is the SiFive CLINT's `msip[hartid]` register —
primary writes 1 to wake the secondary's `wfi`. Capped at 8 harts
(stack reservation in `link.ld`); harts beyond that park silently
without a stack. See [`examples/smp/`](examples/smp/) for a working
demo and [`include/smp.h`](include/smp.h) for the full API.

### Opting out of SMP

Pass `HAL_NO_SMP=1` when building both the HAL and your firmware to
strip multi-hart support entirely. `start.S` shrinks back to the
single-hart shape (any non-zero hart parks forever, no per-hart
stack math), `smp.c` isn't compiled, and `smp.h` becomes a header of
inline stubs (`smp_hart_count() == 1`, `smp_start() == false`) so
the same source compiles either way. Useful for FPGA softcore
deployments where the 128 KiB stack reservation matters, or when
you want the smallest possible boot path.

```sh
make HAL_NO_SMP=1
# in your firmware Makefile:
CFLAGS  += -DHAL_NO_SMP
$(MAKE) -C $(HAL) HAL_NO_SMP=1
```

The flag must be set on **both** sides — the lib build and your
consumer's CFLAGS — otherwise prototypes in `smp.h` and symbols in
`libhal.a` disagree and you get either link errors or surprise
function calls into a no-SMP-stripped lib.

## Real-world consumer

[**SolAstrius/scev-chip-8**](https://github.com/SolAstrius/scev-chip-8)
— a complete CHIP-8 interpreter as bare-metal RISC-V firmware,
built on top of this HAL. ~480 lines of CHIP-8-specific code
(interpreter, ROM, orchestration); everything else (UART, FDT, PCI,
Bochs Display, I²C, HID, ATA) comes from rvvm-hal. Boots with the
embedded IBM Logo splash, or loads any `.ch8` ROM mounted via
`-ata`. Keyboard input via the GUI window is wired through HID.

It pins this repo via submodule, so it's a working reference for
the consumption pattern in the previous section.

## Versioning

Git-tagged. Pin a specific version in your submodule for stability:

```sh
git -C vendor/rvvm-hal checkout v0.1.0
git add vendor/rvvm-hal && git commit -m "pin rvvm-hal v0.1.0"
```

## License

MIT-ish. Treat as public-domain reference code.

The RVVM-side device emulators it talks to live in `LekKit/RVVM`
under MPL-2.0 — the address constants and register layouts in
`include/rvvm.h` were derived from reading that source. RVVM itself
isn't redistributed here.
