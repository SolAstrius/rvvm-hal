#include "irq.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"
#include <stddef.h>

/* M-mode external interrupt cause code (mcause low bits, with bit 63
 * set when mcause's high bit indicates "interrupt"). */
#define MCAUSE_INTERRUPT_BIT   (1ULL << 63)
#define MCAUSE_M_SOFTWARE      3
#define MCAUSE_M_TIMER         7
#define MCAUSE_M_EXTERNAL      11

/* mstatus / mie bits we care about. */
#define MSTATUS_MIE            (1ULL << 3)
#define MIE_MEIE               (1ULL << 11)
#define MIE_MTIE               (1ULL <<  7)
#define MIE_MSIE               (1ULL <<  3)

/* Single-hart firmware: M-mode context number is always 0
 * (ctx = (hartid<<1) | mode_s). riscv-plic.c:51-54. */
#define PLIC_CTX_M_HART0       0

extern void __trap_entry(void);   /* start.S */

static uintptr_t plic_base = RVVM_PLIC_BASE;

/* Dispatch table — one slot per IRQ source. Index 0 is "no source"
 * per PLIC convention (claim returns 0 when nothing pending). */
static struct {
    irq_handler_t fn;
    void         *ctx;
    uint32_t      count;
} handlers[RVVM_PLIC_SRC_LIMIT];

static uint32_t total_irqs = 0;

/* PLIC register helpers. Every PLIC register is naturally 32-bit. */
static inline uintptr_t plic_prio_reg(uint32_t source) {
    return plic_base + RVVM_PLIC_PRIO_OFF + source * 4;
}

static inline uintptr_t plic_enable_reg(uint32_t ctx, uint32_t word_idx) {
    return plic_base + RVVM_PLIC_ENABLE_OFF
         + ctx * RVVM_PLIC_ENABLE_STRIDE
         + word_idx * 4;
}

static inline uintptr_t plic_ctx_reg(uint32_t ctx, uint32_t flag_off) {
    return plic_base + RVVM_PLIC_CTX_OFF
         + ctx * RVVM_PLIC_CTX_STRIDE
         + flag_off;
}

/* Read-modify-write of an enable bit. PLIC enable bits live in 32-bit
 * words, 32 sources per word, indexed by source >> 5. */
static void plic_enable_bit(uint32_t source, bool enable) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    uintptr_t reg = plic_enable_reg(PLIC_CTX_M_HART0, source >> 5);
    uint32_t  cur = mmio_r32(reg);
    uint32_t  bit = 1u << (source & 0x1F);
    mmio_w32(reg, enable ? (cur | bit) : (cur & ~bit));
}

void irq_init(uintptr_t base) {
    if (base) plic_base = base;

    for (uint32_t i = 0; i < RVVM_PLIC_SRC_LIMIT; i++) {
        handlers[i].fn    = NULL;
        handlers[i].ctx   = NULL;
        handlers[i].count = 0;
    }
    total_irqs = 0;

    /* Disable every source on our context, zero priorities, threshold 0. */
    for (uint32_t word = 0; word < (RVVM_PLIC_SRC_LIMIT + 31) / 32; word++) {
        mmio_w32(plic_enable_reg(PLIC_CTX_M_HART0, word), 0);
    }
    for (uint32_t i = 1; i < RVVM_PLIC_SRC_LIMIT; i++) {
        mmio_w32(plic_prio_reg(i), 0);
    }
    mmio_w32(plic_ctx_reg(PLIC_CTX_M_HART0, RVVM_PLIC_CTX_THRESHOLD), 0);

    /* Point mtvec at our trampoline, direct mode (low 2 bits = 0). */
    uintptr_t base_addr = (uintptr_t)&__trap_entry;
    __asm__ volatile ("csrw mtvec, %0" :: "r"(base_addr));

    /* Enable M-external in mie. mstatus.MIE stays off until
     * irq_global_enable() — caller wires handlers first. */
    uintptr_t mie_bits = MIE_MEIE;
    __asm__ volatile ("csrs mie, %0" :: "r"(mie_bits));
}

void irq_register(uint32_t source, irq_handler_t handler, void *ctx) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    handlers[source].fn  = handler;
    handlers[source].ctx = ctx;
}

void irq_enable(uint32_t source) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    /* Default the priority to 1 if it's still 0 — enabling without a
     * priority would make the IRQ pend in PLIC but never deliver. */
    if (mmio_r32(plic_prio_reg(source)) == 0) {
        mmio_w32(plic_prio_reg(source), 1);
    }
    plic_enable_bit(source, true);
}

void irq_disable(uint32_t source) {
    plic_enable_bit(source, false);
}

void irq_set_priority(uint32_t source, uint32_t prio) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    mmio_w32(plic_prio_reg(source), prio & 0x7);
}

void irq_set_threshold(uint32_t threshold) {
    mmio_w32(plic_ctx_reg(PLIC_CTX_M_HART0, RVVM_PLIC_CTX_THRESHOLD),
             threshold & 0x7);
}

bool irq_global_enable(void) {
    uintptr_t prev;
    __asm__ volatile ("csrrs %0, mstatus, %1" : "=r"(prev) : "r"(MSTATUS_MIE));
    return (prev & MSTATUS_MIE) != 0;
}

bool irq_global_disable(void) {
    uintptr_t prev;
    __asm__ volatile ("csrrc %0, mstatus, %1" : "=r"(prev) : "r"(MSTATUS_MIE));
    return (prev & MSTATUS_MIE) != 0;
}

uint32_t irq_count_total(void) {
    return total_irqs;
}

uint32_t irq_count_for(uint32_t source) {
    if (source >= RVVM_PLIC_SRC_LIMIT) return 0;
    return handlers[source].count;
}

/* ====================================================================
 *  Trap dispatcher — called from __trap_entry in start.S.
 *
 *  `regs` points at the 32*8 GPR save area. Layout per __trap_entry:
 *  index N = xN; xN holds entry-time value of the corresponding register
 *  (with x0 at index 1 unused, and x2 holding the *pre-decrement* sp).
 * ==================================================================== */

/* Frame indices match register numbers. */
#define R_SP   2

static void panic_exception(uint64_t mcause, uint64_t mepc, uint64_t *regs) {
    static const char *exc_names[16] = {
        "instruction misaligned", "instruction access fault",
        "illegal instruction",    "breakpoint",
        "load misaligned",        "load access fault",
        "store misaligned",       "store access fault",
        "ecall U",                "ecall S",
        "reserved (10)",          "ecall M",
        "instruction page fault", "load page fault",
        "reserved (14)",          "store page fault",
    };
    const char *name = (mcause < 16) ? exc_names[mcause] : "unknown";
    uart_printf("\n!! TRAP: %s (mcause=%x)\n", name, mcause);
    uart_printf("   mepc=%p  ra=%p  sp=%p\n",
                (void *)(uintptr_t)mepc,
                (void *)(uintptr_t)regs[1],
                (void *)(uintptr_t)regs[R_SP]);
    uart_printf("   a0=%x  a1=%x  a2=%x  a3=%x\n",
                regs[10], regs[11], regs[12], regs[13]);
    uart_puts("Halted.\n");
    for (;;) __asm__ volatile ("wfi");
}

static void handle_external(void) {
    /* Drain — multiple sources can be claim-ready at once. PLIC's
     * claim register returns 0 when there's nothing left. */
    for (;;) {
        uintptr_t claim_reg = plic_base + RVVM_PLIC_CTX_OFF
                            + PLIC_CTX_M_HART0 * RVVM_PLIC_CTX_STRIDE
                            + RVVM_PLIC_CTX_CLAIM;
        uint32_t source = mmio_r32(claim_reg);
        if (source == 0) break;
        if (source < RVVM_PLIC_SRC_LIMIT) {
            handlers[source].count++;
            total_irqs++;
            if (handlers[source].fn) {
                handlers[source].fn(source, handlers[source].ctx);
            }
        }
        /* Complete = write the same source ID back. Pending bit is
         * cleared by the claim itself; complete tells PLIC we can
         * re-deliver this source if it stays asserted. */
        mmio_w32(claim_reg, source);
    }
}

void trap_dispatch(uint64_t mcause, uint64_t mepc, uint64_t *regs) {
    if (mcause & MCAUSE_INTERRUPT_BIT) {
        uint64_t code = mcause & ~MCAUSE_INTERRUPT_BIT;
        switch (code) {
        case MCAUSE_M_EXTERNAL:
            handle_external();
            return;
        case MCAUSE_M_TIMER:
            /* Caller hasn't wired a timer handler — disable MTIE so we
             * don't loop. The timer can be turned back on by writing
             * MTIMECMP and re-setting mie.MTIE. */
            uart_puts("irq: M-timer fired with no handler; disabling MTIE\n");
            __asm__ volatile ("csrc mie, %0" :: "r"(MIE_MTIE));
            return;
        case MCAUSE_M_SOFTWARE:
            uart_puts("irq: M-software IRQ; disabling MSIE\n");
            __asm__ volatile ("csrc mie, %0" :: "r"(MIE_MSIE));
            return;
        default:
            uart_printf("irq: unhandled interrupt code %u\n", code);
            return;
        }
    }
    /* Synchronous exception — panic. */
    panic_exception(mcause, mepc, regs);
}
