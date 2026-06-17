# Device tree — RVVM vs QEMU virt

Snapshots captured by running `examples/probe-s` on each host. The
firmware does a token-level FDT walk and prints every node with its
first `compatible` string and root `reg` entry, so you can re-capture
fresh dumps anytime by re-running the example. RVVM's PCI endpoints
(Bochs, HDA, RTL8169, ATA) don't appear here because RVVM doesn't
emit FDT nodes for them — guests scan the PCI bus to discover them.

To regenerate:

```sh
make run-qemu-probe-s   2>&1 | sed -n '/Full FDT walk:/,$p' > docs/fdt-qemu-virt.txt
make -C examples/probe-s run | sed -n '/Full FDT walk:/,$p' > docs/fdt-rvvm.txt
```

## Diff (by compatible string)

| compatible | RVVM | QEMU virt | notes |
|---|:---:|:---:|---|
| `ns16550a` | ✓ | ✓ | UART — both at `0x10000000` |
| `sifive,clint0` | ✓ | ✓ | both at `0x02000000` |
| `sifive,plic-1.0.0` | ✓ | ✓ | both at `0x0c000000`; RVVM advertises 64 MiB region, QEMU 6 MiB (real layout 6 MiB on both) |
| `pci-host-ecam-generic` | ✓ | ✓ | both at `0x30000000`, 256 MiB ECAM window |
| `google,goldfish-rtc` | ✓ | ✓ | both at `0x00101000` |
| `sifive,test1` (syscon) | ✓ | ✓ | both at `0x00100000`; poweroff=0x5555 / reset=0x7777 |
| `syscon-poweroff` / `syscon-reboot` | ✓ | ✓ | reference the syscon node above |
| `simple-bus` | ✓ | ✓ | `/soc` parent |
| `riscv,cpu-intc` | ✓ | ✓ | per-hart interrupt controller |
| `opencores,i2c-ocores` | ✓ | – | RVVM-only; the i2c-HID keyboard / mouse / tablet hang off this. QEMU's HID story is virtio-input. |
| `fixed-clock` | ✓ | – | i2c oscillator clock node on RVVM |
| `lekkit,rvvm` | ✓ | – | root + cpu compat string |
| `virtio,mmio` × 8 | – | ✓ | QEMU's main device transport; driven by `virtio.c` (gpu/input backends) |
| `cfi-flash` | – | ✓ | parallel-flash NOR @ `0x20000000` (32 MiB × 2 banks) |
| `qemu,fw-cfg-mmio` | – | ✓ | QEMU fw-cfg @ `0x10100000` |
| `qemu,platform` (simple-bus) | – | ✓ | QEMU device-bus container @ `0x04000000` |
| `riscv,pmu` | – | ✓ | PMU event mapping |
| `riscv-virtio` | – | ✓ | root compat |
| `riscv` | – | ✓ | cpu compat (RVVM uses `lekkit,rvvm` here instead) |

## Same binary, two hosts

Most HAL drivers default to the hardcoded `RVVM_*_BASE` constants in
`include/rvvm.h` (those addresses are identical on both hosts). Drivers
that genuinely match a compatible string at runtime — uart, clint/smp,
plic, pci, i2c-HID (`opencores,i2c-ocores`/`hid-over-i2c`), and the
virtio/cfi/fw-cfg trio — auto-bind via FDT; others (RTC, syscon) just
poke their fixed base. So:

- **Drivers in the "both" rows** work identically on either host —
  this is the `examples/probe-s` cross-host smoke path.
- **Drivers in "RVVM only" rows** (i2c-HID via opencores-i2c) silently
  no-op on QEMU because the FDT compatible doesn't match.
- **"QEMU only" devices** (virtio-mmio, fw-cfg, cfi-flash) are now
  driven by `virtio.c`, `fw_cfg.c`, and `cfi.c` respectively — each
  auto-disables on RVVM when its compatible string is absent.

## Captured dumps

- [`fdt-rvvm.txt`](fdt-rvvm.txt) — RVVM default machine
- [`fdt-qemu-virt.txt`](fdt-qemu-virt.txt) — `qemu-system-riscv64 -M virt`
