/* smp — multi-hart detection + secondary wake-up demo.
 *
 * Boots, walks the FDT, counts harts via /cpus children with
 * device_type = "cpu", then walks the secondary harts in turn:
 * smp_start each one with a function that prints a greeting via
 * the UART, smp_wait for it to return, move on.
 *
 * Run with -smp N:
 *   nix develop ../.. -c make
 *   rvvm firmware.bin -nogui -nonet -smp 4
 *
 * Expected output: one greeting per non-primary hart (so 3 lines for
 * -smp 4), each containing the hart's mhartid. With -smp 1 (or no
 * -smp flag) the loop body never runs. */

#include "uart.h"
#include "fdt.h"
#include "pci.h"
#include "time.h"
#include "smp.h"
#include "rvvm.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

extern char __bss_start[], __bss_end[];

/* Runs on the secondary hart. The argument is just a sentinel value
 * to demonstrate that arg threads through end-to-end; it has no
 * other meaning.
 *
 * UART access from a secondary is racy with the primary — they share
 * the same NS16550A. For this demo we sequence smp_start + smp_wait
 * one hart at a time so only one hart is touching the UART at any
 * moment. A more aggressive parallel firmware would either lock the
 * UART or have secondaries communicate results via a shared buffer
 * the primary then prints. */
static void greet(uint64_t hartid, void *arg) {
    uart_printf("  [hart %u] hello! arg=%p, mhartid via CSR = %u\n",
                hartid, arg, (uint64_t)smp_this_hart());
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== smp: multi-hart detection demo ===\n");
    uart_printf("primary hart=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    fdt_t fdt;
    if (!fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uart_puts("FDT invalid; aborting.\n");
        for (;;) __asm__ volatile ("wfi");
    }

    /* Re-init UART + CLINT/timer from FDT addresses (so this demo
     * works on non-default RVVM machines where the bases drift). */
    uint32_t u_off = fdt_find_compatible(&fdt, "ns16550a");
    uint32_t c_off = fdt_find_compatible(&fdt, "sifive,clint0");
    uint64_t u_at = 0, c_at = 0;
    fdt_node_reg64(&fdt, u_off, 0, &u_at, NULL);
    fdt_node_reg64(&fdt, c_off, 0, &c_at, NULL);
    uart_init((uintptr_t)u_at);
    uint32_t cpus = fdt_find_node_named(&fdt, "cpus");
    uint32_t hz = 0;
    if (cpus != UINT32_MAX) fdt_node_prop_u32(&fdt, cpus, "timebase-frequency", &hz);
    time_init((uintptr_t)c_at, hz);

    smp_init(&fdt);
    uint32_t n = smp_hart_count();
    uart_printf("\nsmp: %u hart%s detected via /cpus\n",
                (uint64_t)n, n == 1 ? "" : "s");

    if (n == 1) {
        uart_puts("(start RVVM with -smp N to bring up secondary harts)\n");
    } else {
        uart_puts("\nwaking secondaries one at a time:\n");
        for (uint32_t h = 1; h < n; h++) {
            uart_printf("[primary] starting hart %u...\n", (uint64_t)h);
            if (!smp_start(h, greet, (void *)(uintptr_t)0xC0FFEE)) {
                uart_printf("  smp_start(%u) refused\n", (uint64_t)h);
                continue;
            }
            smp_wait(h);
            uart_printf("[primary] hart %u parked again.\n", (uint64_t)h);
        }

        /* Round 2: fire all secondaries off concurrently and wait
         * once at the end. UART access overlaps so output may
         * interleave by character — that's the visible signature
         * of true parallel execution. */
        uart_puts("\nround 2: all secondaries concurrent (output may interleave):\n");
        for (uint32_t h = 1; h < n; h++) {
            smp_start(h, greet, (void *)(uintptr_t)0xBEEF);
        }
        for (uint32_t h = 1; h < n; h++) {
            smp_wait(h);
        }
        uart_puts("[primary] all secondaries returned.\n");
    }

    uart_puts("\ndone.\n");
    for (;;) __asm__ volatile ("wfi");
}
