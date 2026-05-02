# rust-hello

Rust port of the canonical rvvm-hal hello-world: boots, brings up the
UART, prints a banner, exits cleanly via `hal_exit`. The same firmware
runs unchanged on RVVM and QEMU virt — this example exercises both.

Demonstrates the minimum needed to consume `libhal.a` from a Rust crate:

- `#![no_std]` `#![no_main]`, `riscv64gc-unknown-none-elf` target
- HAL's `start.S` provides `_start`; Rust exports `kmain(hartid, fdt_addr)`
  with the C ABI
- `extern "C"` declarations for the few HAL functions we touch
- `#[panic_handler]` routes Rust panics into `hal_panic`, so the
  `!!HAL-PANIC-V1` dumper and `tools/decode-panic` work uniformly for
  Rust- and C-originated faults
- `build.rs` points the linker at `../../libhal.a`; `.cargo/config.toml`
  passes HAL's `link.ld` as `-T` and sets `code-model=medium` for medany

## Build

```sh
nix develop ../.. --command make
```

Produces `firmware.bin` (~4 KiB stripped). The Makefile builds
`../../libhal.a` first, then `cargo build --release`, then
`llvm-objcopy -O binary`.

## Run

```sh
nix develop ../.. --command make run        # → RVVM
nix develop ../.. --command make run-qemu   # → QEMU virt -bios none
```

Both should produce identical stdout:

```
hello from rust on rvvm bare metal!
  hartid    = 0
  fdt_addr  = 0x...
  language  = rust (no_std)

!!HAL-EXIT code=0
```

## What's not here

- **No `alloc`.** `core` only — no `Box`, `Vec`, `String`. For an
  allocator-backed Rust firmware add a `#[global_allocator]` over the
  `__heap_start` / `__heap_end` linker symbols (e.g. `talc` or
  `linked_list_allocator`); HAL itself does not require picolibc for
  this — the heap symbols are reserved unconditionally.
- **No bindgen.** The handful of HAL functions we need are hand-declared
  inline. For a larger Rust consumer, run `bindgen` against
  `../../include/*.h` and commit the result.
- **No bit-manip codegen.** `riscv64gc-unknown-none-elf` is rv64gc only;
  the HAL's default `HAL_MARCH_EXTS` adds zba/zbb/zbs/zicond/zicboz on
  the C side. To get those from rustc as well you'd need a custom
  `target.json` with `"features": "+zba,+zbb,+zbs,+zicond"` and
  `-Zbuild-std=core`. Codegen difference is small for a hello-world.
