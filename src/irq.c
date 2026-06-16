/* irq.c — privilege-portable interrupt subsystem.
 *
 * Wires CSR_TVEC to a register-saving trampoline (start.S), forwards
 * to a C dispatcher, and routes external interrupts through the
 * platform's claim/complete protocol.
 *
 * What lives here vs. in plat_*.c:
 *   - here: trap-vector setup, dispatch table, claim/complete loop,
 *     mode-aware status/ie bit toggles via priv.h aliases.
 *   - plat: per-controller MMIO (PLIC vs CLIC vs IMSIC), CSR-name
 *     resolution comes from priv.h.
 *
 * The dispatch table is sized to the platform's source limit
 * (plat_intc_source_limit) but we cap that at IRQ_HANDLERS_MAX to keep
 * the BSS footprint bounded — RVVM's PLIC tops out at 64. Anything
 * larger (AIA IMSIC supports thousands of EIDs) needs a sparser
 * structure; revisit when that backend lands. */

#include "irq.h"
#include "plat.h"
#include "priv.h"
#include "rvvm.h"
#include "uart.h"
#include "panic.h"
#include <stddef.h>

#define IRQ_HANDLERS_MAX  RVVM_PLIC_SRC_LIMIT

extern void __trap_entry(void);   /* start.S */

static struct {
    irq_handler_t fn;
    void         *ctx;
    uint32_t      count;
} handlers[IRQ_HANDLERS_MAX];

static uint32_t total_irqs = 0;

/* Timer + software/IPI handlers. NULL → dispatcher's built-in default
 * for that cause (see irq.h). */
static irq_local_handler_t timer_fn  = NULL;
static void               *timer_ctx = NULL;
static irq_local_handler_t ipi_fn    = NULL;
static void               *ipi_ctx   = NULL;

void irq_init(uintptr_t plic_base) {
    /* plat_init has been called already by kmain (it discovers the
     * controller base from FDT). The legacy `plic_base` argument here
     * is kept for ABI: callers historically passed an FDT-derived
     * address into irq_init. Forward it to plat_init so backends that
     * weren't initialised yet still pick it up. */
    if (plic_base) plat_init(plic_base, 0);

    for (uint32_t i = 0; i < IRQ_HANDLERS_MAX; i++) {
        handlers[i].fn    = NULL;
        handlers[i].ctx   = NULL;
        handlers[i].count = 0;
    }
    total_irqs = 0;

    plat_intc_init();

    /* Point trap vector at our trampoline, direct mode (low 2 bits = 0). */
    uintptr_t base_addr = (uintptr_t)&__trap_entry;
    __asm__ volatile ("csrw " HAL_CSR(CSR_TVEC) ", %0" :: "r"(base_addr));

    /* Enable the external-interrupt bit in CSR_IE (mie.MEIE / sie.SEIE).
     * Global enable (CSR_STATUS interrupt-enable bit) stays off until
     * irq_global_enable() — caller wires handlers first. */
    uintptr_t bit = IE_EXT_BIT;
    __asm__ volatile ("csrs " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

void irq_register(uint32_t source, irq_handler_t handler, void *ctx) {
    if (source == 0 || source >= IRQ_HANDLERS_MAX) return;
    handlers[source].fn  = handler;
    handlers[source].ctx = ctx;
}

void irq_register_timer(irq_local_handler_t handler, void *ctx) {
    timer_fn  = handler;
    timer_ctx = ctx;
}

void irq_register_ipi(irq_local_handler_t handler, void *ctx) {
    ipi_fn  = handler;
    ipi_ctx = ctx;
}

void irq_ipi_enable(void) {
    uintptr_t bit = IE_SOFT_BIT;
    __asm__ volatile ("csrs " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

void irq_ipi_disable(void) {
    uintptr_t bit = IE_SOFT_BIT;
    __asm__ volatile ("csrc " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

void irq_enable(uint32_t source) {
    plat_intc_enable_source(source);
}

void irq_disable(uint32_t source) {
    plat_intc_disable_source(source);
}

void irq_set_priority(uint32_t source, uint32_t prio) {
    plat_intc_set_priority(source, prio);
}

void irq_set_threshold(uint32_t threshold) {
    plat_intc_set_threshold(threshold);
}

bool irq_global_enable(void) {
    uintptr_t prev, bit = STATUS_IE_BIT;
    __asm__ volatile ("csrrs %0, " HAL_CSR(CSR_STATUS) ", %1"
                      : "=r"(prev) : "r"(bit));
    return (prev & STATUS_IE_BIT) != 0;
}

bool irq_global_disable(void) {
    uintptr_t prev, bit = STATUS_IE_BIT;
    __asm__ volatile ("csrrc %0, " HAL_CSR(CSR_STATUS) ", %1"
                      : "=r"(prev) : "r"(bit));
    return (prev & STATUS_IE_BIT) != 0;
}

uint32_t irq_count_total(void) {
    return total_irqs;
}

uint32_t irq_count_for(uint32_t source) {
    if (source >= IRQ_HANDLERS_MAX) return 0;
    return handlers[source].count;
}

/* ====================================================================
 *  Trap dispatcher
 *
 *  Called from __trap_entry (start.S). `regs` points at the 32×8 GPR
 *  save area; index N = xN; slot 0 unused; slot 2 holds the entry-time
 *  sp (pre-decrement).
 *
 *  Argument names retain their M-mode spelling (mcause/mepc) for ABI
 *  stability with hal_panic_from_trap(). The values are whichever
 *  cause/epc CSR was active for the current privilege mode.
 * ==================================================================== */

static void handle_external(void) {
    /* Drain — multiple sources can be claim-ready at once. plat returns
     * 0 when nothing is pending. */
    for (;;) {
        uint32_t source = plat_intc_claim();
        if (source == 0) break;
        if (source < IRQ_HANDLERS_MAX) {
            handlers[source].count++;
            total_irqs++;
            if (handlers[source].fn) {
                handlers[source].fn(source, handlers[source].ctx);
            }
        }
        plat_intc_complete(source);
    }
}

void trap_dispatch(uint64_t mcause, uint64_t mepc, uint64_t *regs) {
    if (mcause & CAUSE_INTERRUPT_BIT) {
        uint64_t code = mcause & ~CAUSE_INTERRUPT_BIT;
        uintptr_t bit;
        if (code == CAUSE_EXTERNAL) {
            handle_external();
            return;
        } else if (code == CAUSE_TIMER) {
            total_irqs++;
            if (timer_fn) {
                /* Registered handler owns the deadline: it re-arms for a
                 * periodic tick (plat_timer_set_deadline) or disarms to
                 * stop. The dispatcher deliberately doesn't touch the
                 * comparator or IE bit so the handler stays in control. */
                timer_fn(timer_ctx);
                return;
            }
            /* No handler — disable the timer IE bit so a stray deadline
             * can't loop. Re-arm + plat_timer_irq_enable turns it back
             * on. (time.c's wfi-pacing relies on this default.) */
            uart_puts("irq: timer fired with no handler; disabling timer IE\n");
            bit = IE_TIMER_BIT;
            __asm__ volatile ("csrc " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
            return;
        } else if (code == CAUSE_SOFTWARE) {
            /* Acknowledge (clear) the IPI on this hart first — that drops
             * the pending state so we don't re-trap — then run the
             * registered handler, if any. plat_ipi_ack alone breaks the
             * storm, so a handler-less IPI is a no-op wake (several use
             * cases — cross-hart sync, benchmarks — want exactly that). */
            plat_ipi_ack(plat_this_hart());
            total_irqs++;
            if (ipi_fn) ipi_fn(ipi_ctx);
            return;
        } else {
            uart_printf("irq: unhandled interrupt code %u\n", code);
            return;
        }
    }
    /* Synchronous exception — panic. */
    hal_panic_from_trap(mcause, mepc, regs);
}
