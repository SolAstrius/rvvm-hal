/* probe-s — minimal first-light test for the S-mode + SBI HAL build.
 *
 * Booted by OpenSBI as the next-stage S-mode payload. We print a
 * banner via UART (NS16550A is mapped to S-mode by RVVM, same as in
 * M-mode), walk the FDT to confirm the same machine layout we know
 * from the M-mode probe, exercise plat_timer (one wfi-paced sleep),
 * and shut down via the SBI SRST extension.
 *
 * RAM start is 0x80200000 (link_s.ld). OpenSBI lives at 0x80000000.
 *
 * Run: rvvm fw_dynamic.bin -k firmware.bin -nogui -nonet -nosound
 *      RVVM passes `firmware.bin` to OpenSBI as the M-mode firmware
 *      and `-k firmware.bin` is loaded at RAM+0x200000 = 0x80200000,
 *      which OpenSBI fw_jump targets by default. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "plat.h"
#include "rvvm.h"
#include <stdint.h>
#include <stddef.h>

extern char __bss_start[], __bss_end[];

/* SBI System Reset extension — used to power off cleanly so the host
 * RVVM exits 0. EID 0x53525354 ("SRST"), FID 0 = sbi_system_reset. */
static void sbi_shutdown(void) {
    register unsigned long a0 __asm__("a0") = 0;   /* type=shutdown */
    register unsigned long a1 __asm__("a1") = 0;   /* reason=none   */
    register unsigned long a6 __asm__("a6") = 0;
    register unsigned long a7 __asm__("a7") = 0x53525354UL;
    __asm__ volatile ("ecall"
                      : "+r"(a0), "+r"(a1)
                      : "r"(a6), "r"(a7)
                      : "memory");
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== rvvm-hal probe-s (S-mode under SBI) ===\n");
    uart_printf("hart=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* Read sstatus to prove we're in S-mode (SPP bit reflects the
     * privilege we trapped *from*; checking it isn't useful here, but
     * reading sstatus at all would have trapped in M-mode if we were
     * confused about the build). */
    uint64_t sstatus;
    __asm__ volatile ("csrr %0, sstatus" : "=r"(sstatus));
    uart_printf("sstatus = %x  (csrr sstatus succeeded → we are in S-mode)\n",
                sstatus);

    fdt_t fdt;
    if (!fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uart_puts("FDT invalid; aborting.\n");
        sbi_shutdown();
        for (;;) __asm__ volatile ("wfi");
    }

    /* Hand the FDT to the platform layer for runtime feature probes
     * (Sstc → direct stimecmp path, skipping sbi_set_timer ecalls). */
    plat_probe_features(&fdt);

    /* List the same handful of devices the M-mode probe lists; we
     * expect the FDT layout to be identical. */
    static const char *want[] = {
        "ns16550a", "sifive,plic-1.0.0", "sifive,clint0",
        "google,goldfish-rtc", "syscon", NULL,
    };
    uart_puts("Devices visible via FDT:\n");
    for (const char **p = want; *p; p++) {
        uint32_t off = fdt_find_compatible(&fdt, *p);
        if (off == UINT32_MAX) {
            uart_printf("  (none)  %s\n", *p);
            continue;
        }
        uint64_t at = 0, sz = 0;
        fdt_node_reg64(&fdt, off, 0, &at, &sz);
        uart_printf("  %s @ %p  size=%x\n",
                    *p, (void *)(uintptr_t)at, sz);
    }

    /* Bring up time subsystem from FDT. SBI sets the timer; we just
     * need timebase-frequency from /cpus to scale frame deadlines. */
    uint32_t cpus_off = fdt_find_node_named(&fdt, "cpus");
    uint32_t hz = 0;
    if (cpus_off != UINT32_MAX)
        fdt_node_prop_u32(&fdt, cpus_off, "timebase-frequency", &hz);
    time_init(0, hz);
    uart_printf("\ntime: %u Hz; time_now() = %x\n",
                (uint64_t)time_hz(), time_now());

    /* Sleep one frame using plat_timer + wfi. Confirms the timer path
     * works and that sip.STIP wakes wfi. With Sstc, the deadline is
     * armed via direct csrw stimecmp; otherwise via sbi_set_timer
     * ecall. The probe-s output overhead per frame should drop
     * noticeably with Sstc on. */
    uart_puts("time: sleeping one frame...\n");
    uint64_t before = time_now();
    time_busy_until(before + time_ticks_per_frame());
    uint64_t after = time_now();
    uart_printf("time: woke after %u ticks (target %u, overhead %u)\n",
                after - before, time_ticks_per_frame(),
                (after - before) - time_ticks_per_frame());

    /* Repeat 50× to amortise out the print latency in the first
     * sample, giving a steadier average. */
    uint64_t total_overhead = 0;
    const uint32_t reps = 50;
    for (uint32_t i = 0; i < reps; i++) {
        uint64_t t0 = time_now();
        time_busy_until(t0 + time_ticks_per_frame());
        uint64_t t1 = time_now();
        total_overhead += (t1 - t0) - time_ticks_per_frame();
    }
    uart_printf("time: avg overhead over %u frames = %u ticks (%u us)\n",
                (uint64_t)reps,
                total_overhead / reps,
                (total_overhead / reps) / 10);   /* 10 MHz → 1 tick = 100 ns */

    uart_puts("\nprobe-s: clean shutdown via SBI SRST.\n");
    sbi_shutdown();
    for (;;) __asm__ volatile ("wfi");
}
