// rust-hello — Rust port of the canonical rvvm-hal hello-world.
//
// Boots, brings up the UART, prints a banner, parks the hart in wfi.
// Rust panics route through hal_panic so the !!HAL-PANIC-V1 dump and
// tools/decode-panic work without modification.
//
// Build flow lives in the sibling Makefile; this file is just the
// firmware payload. See README in this dir for run instructions.

#![no_std]
#![no_main]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

// HAL surface we touch. The HAL's headers are C — we hand-write the
// few prototypes we need rather than running bindgen for a hello-world.
// uart_puts / hal_panic / hal_exit are stable across HAL minor versions.
unsafe extern "C" {
    fn uart_init(base: usize);
    fn uart_puts(s: *const c_char);
    fn uart_printf(fmt: *const c_char, ...);
    fn hal_panic(fmt: *const c_char, ...) -> !;
    fn hal_exit(code: c_int) -> !;
}

// Entry point that HAL's start.S tail-calls after parking secondaries
// and zeroing BSS. Signature must match `void kmain(uint64_t hartid,
// uint64_t fdt_addr)` — a0/a1 in the C ABI.
//
// `extern "C"` for the calling convention; `no_mangle` so the linker
// finds it under the unadorned symbol name start.S references.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kmain(hartid: u64, fdt_addr: u64) -> ! {
    unsafe {
        // 0 = use RVVM's NS16550A default (0x10000000). A real driver
        // would FDT-discover the base; for hello-world the default is
        // fine and matches QEMU virt's layout too.
        uart_init(0);
        uart_puts(c"hello from rust on rvvm bare metal!\n".as_ptr());
        uart_printf(
            c"  hartid    = %u\n  fdt_addr  = %p\n  language  = rust (no_std)\n".as_ptr(),
            hartid,
            fdt_addr as *const u8,
        );
        // Clean RVVM exit via SYSCON_POWEROFF — same sentinel the C
        // examples use, so `rvvm` returns to the host shell instead
        // of spinning.
        hal_exit(0);
    }
}

// Rust panic handler — funnel into the HAL's structured dumper so all
// panics (Rust *and* C) emit the same V1 frame and decode-panic
// symbolicates them identically.
#[panic_handler]
fn on_panic(info: &PanicInfo) -> ! {
    // We can't easily format the PanicInfo without alloc, but the file
    // and line are useful on their own. Pass them as a static format
    // string so we don't need to reach for a formatter.
    let loc = info.location();
    unsafe {
        match loc {
            Some(l) => hal_panic(
                c"rust panic at %s:%u\n".as_ptr(),
                l.file().as_ptr(),
                l.line(),
            ),
            None => hal_panic(c"rust panic (no location)\n".as_ptr()),
        }
    }
}
