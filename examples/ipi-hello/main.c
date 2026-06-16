/* ipi-hello — custom software-interrupt (IPI) handler smoke test.
 *
 * Software interrupts are the third RISC-V interrupt cause (alongside
 * external/PLIC and timer). On the M-mode+CLINT machine an IPI is raised
 * by writing CLINT msip[hart] — plat_ipi_send() does that. With
 * mstatus.MIE and mie.MSIE on it traps into the dispatcher, which acks
 * the pending bit and calls a handler registered via irq_register_ipi().
 *
 * This boots, registers a custom IPI handler, enables software-interrupt
 * delivery, sends a few self-IPIs and confirms the handler ran for each,
 * then unregisters it and sends one more to show the dispatcher's default
 * (ack-only) path leaves the count untouched.
 *
 *   make
 *   make run
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "uart.h"
#include "fdt.h"
#include "irq.h"
#include "plat.h"
#include "rvvm.h"

extern char __bss_start[], __bss_end[];

static volatile uint32_t ipi_count;

/* Custom software-interrupt handler. Runs in interrupt context after the
 * dispatcher has already acked the IPI, so it just does its own work. */
static void on_ipi(void *ctx) {
    ipi_count++;
    uart_printf("  [ipi handler] fired #%u, ctx=%p\n",
                (uint64_t)ipi_count, ctx);
}

/* Send one self-IPI and spin (bounded) until the handler observes it, so
 * the demo is deterministic rather than racing the trap. */
static bool ping_self(uint32_t expect) {
    uint32_t self = plat_this_hart();
    plat_ipi_send(self);
    for (volatile uint32_t i = 0; i < 1000000u; i++) {
        if (ipi_count >= expect) return true;
    }
    return false;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== ipi-hello (custom software-interrupt handler) ===\n");

    fdt_t fdt;
    uint64_t plic_at = 0, clint_at = 0;
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t off = fdt_find_compatible(&fdt, "ns16550a");
        uint64_t at = 0;
        if (off != UINT32_MAX && fdt_node_reg64(&fdt, off, 0, &at, NULL)) {
            uart_init((uintptr_t)at);
        }
        off = fdt_find_compatible(&fdt, "sifive,plic-1.0.0");
        if (off != UINT32_MAX) fdt_node_reg64(&fdt, off, 0, &plic_at, NULL);
        off = fdt_find_compatible(&fdt, "sifive,clint0");
        if (off != UINT32_MAX) fdt_node_reg64(&fdt, off, 0, &clint_at, NULL);
    }

    /* irq_init claims the trap vector + arms the external IE bit. The
     * CLINT base (for msip) comes from plat_init; feed it the discovered
     * address so plat_ipi_send pokes the right register. */
    plat_init((uintptr_t)plic_at, (uintptr_t)clint_at);
    irq_init((uintptr_t)plic_at);

    uart_printf("hart=%u  registering custom IPI handler...\n", hartid);
    irq_register_ipi(on_ipi, (void *)(uintptr_t)0xABCD);
    irq_ipi_enable();        /* mie.MSIE — without this an IPI won't trap */
    irq_global_enable();     /* mstatus.MIE */

    uart_puts("\nsending 3 self-IPIs (each should trap into the handler):\n");
    bool ok = true;
    for (uint32_t k = 1; k <= 3; k++) {
        if (!ping_self(k)) { uart_printf("  IPI #%u not observed!\n", (uint64_t)k); ok = false; }
    }
    uart_printf("handler ran %u/3 times -> %s\n",
                (uint64_t)ipi_count, ok ? "OK" : "FAIL");

    uart_puts("\nunregistering handler; sending 1 more (default ack-only path):\n");
    irq_register_ipi(NULL, NULL);
    uint32_t before = ipi_count;
    plat_ipi_send(plat_this_hart());
    for (volatile uint32_t i = 0; i < 1000000u; i++) { (void)i; }
    uart_printf("count stayed at %u (was %u) -> %s\n",
                (uint64_t)ipi_count, (uint64_t)before,
                ipi_count == before ? "OK (no handler called)" : "FAIL");

    uart_printf("\ntotal software irqs dispatched: %u\n", irq_count_total());
    uart_puts("done.\n");
    for (;;) __asm__ volatile ("wfi");
}
