/* plat_m_clint.c — M-mode + CLINT (IPI/timer) + PLIC (intc) backend.
 *
 * The default backend, matching today's RVVM bare-metal target. All
 * three concerns (IPI, timer, intc) talk MMIO directly — no ecalls,
 * no firmware below us.
 *
 * Memory map (set by plat_init from FDT, falling back to RVVM defaults):
 *   CLINT base + hartid*4         msip[hartid]      (IPI)
 *   CLINT base + 0x4000 + hartid*8 mtimecmp[hartid] (timer deadline)
 *   PLIC  base                     priorities/enables/claim
 *
 * Hart context for PLIC is `(hartid << 1) | (mode==S ? 1 : 0)`.
 * In M-mode, hart 0 is context 0. Multi-hart M-mode would index by
 * (hartid << 1); we currently only enable IRQs on the primary (irq.c
 * comment), so context 0 is hardcoded. Once multi-hart IRQ routing is
 * wired the backend will read the calling hart from plat_this_hart(). */

#include "plat.h"
#include "priv.h"
#include "rvvm.h"
#include "mmio.h"
#include <stddef.h>

static uintptr_t plic_base  = RVVM_PLIC_BASE;
static uintptr_t clint_base = RVVM_CLINT_BASE;

/* PLIC context for the calling code. Hardcoded to hart-0 M-mode for
 * now — see file header. */
#define PLIC_CTX_M_HART0  0

/* ====================================================================
 *  Lifecycle
 * ==================================================================== */
void plat_init(uintptr_t intc_base, uintptr_t clint) {
    if (intc_base) plic_base  = intc_base;
    if (clint)     clint_base = clint;
}

uint32_t plat_this_hart(void) {
    uint64_t v;
    __asm__ volatile ("csrr %0, mhartid" : "=r"(v));
    return (uint32_t)v;
}

void plat_hart_bringup_park(uint32_t hartid) {
    /* No-op: start.S already parked every non-primary hart in a
     * wfi/MSIP loop before kmain ran. */
    (void)hartid;
}

void plat_probe_features(const fdt_t *fdt) {
    /* No-op: M+CLINT doesn't have a fast-path mode. mtimecmp MMIO is
     * already the most direct route to the deadline register. */
    (void)fdt;
}

/* ====================================================================
 *  IPI via CLINT msip[hartid]
 * ==================================================================== */
void plat_ipi_send(uint32_t hartid) {
    /* Order: callers will have written shared state (smp_cmd, etc.)
     * before invoking us. The fence makes that visible to the target
     * hart before the MSIP write reaches CLINT. */
    __asm__ volatile ("fence rw,rw" ::: "memory");
    mmio_w32(clint_base + hartid * 4, 1);
}

void plat_ipi_ack(uint32_t self_hartid) {
    mmio_w32(clint_base + self_hartid * 4, 0);
}

/* ====================================================================
 *  Timer via CLINT mtimecmp[hartid]
 *
 *  RVVM gates wfi-wake on mie.MTIE: without that bit the comparator
 *  doesn't drive the wake delay. mstatus.MIE stays off so MTIP wakes
 *  wfi but doesn't trap — caller (time.c) handles the dance.
 * ==================================================================== */
static inline volatile uint64_t *mtimecmp_for(uint32_t hartid) {
    return (volatile uint64_t *)(clint_base + 0x4000 + hartid * 8);
}

void plat_timer_set_deadline(uint64_t deadline) {
    *mtimecmp_for(plat_this_hart()) = deadline;
}

void plat_timer_irq_enable(void) {
    uintptr_t bit = IE_TIMER_BIT;
    __asm__ volatile ("csrs " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

void plat_timer_irq_disable(void) {
    uintptr_t bit = IE_TIMER_BIT;
    __asm__ volatile ("csrc " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

/* ====================================================================
 *  Interrupt controller — SiFive PLIC
 * ==================================================================== */
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

static void plic_enable_bit(uint32_t source, bool enable) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    uintptr_t reg = plic_enable_reg(PLIC_CTX_M_HART0, source >> 5);
    uint32_t  cur = mmio_r32(reg);
    uint32_t  bit = 1u << (source & 0x1F);
    mmio_w32(reg, enable ? (cur | bit) : (cur & ~bit));
}

void plat_intc_init(void) {
    for (uint32_t word = 0; word < (RVVM_PLIC_SRC_LIMIT + 31) / 32; word++) {
        mmio_w32(plic_enable_reg(PLIC_CTX_M_HART0, word), 0);
    }
    for (uint32_t i = 1; i < RVVM_PLIC_SRC_LIMIT; i++) {
        mmio_w32(plic_prio_reg(i), 0);
    }
    mmio_w32(plic_ctx_reg(PLIC_CTX_M_HART0, RVVM_PLIC_CTX_THRESHOLD), 0);
}

void plat_intc_enable_source(uint32_t source) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    /* Default the priority to 1 if it's still 0 — enabling without a
     * priority would make the IRQ pend in PLIC but never deliver. */
    if (mmio_r32(plic_prio_reg(source)) == 0) {
        mmio_w32(plic_prio_reg(source), 1);
    }
    plic_enable_bit(source, true);
}

void plat_intc_disable_source(uint32_t source) {
    plic_enable_bit(source, false);
}

void plat_intc_set_priority(uint32_t source, uint32_t prio) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
    mmio_w32(plic_prio_reg(source), prio & 0x7);
}

void plat_intc_set_threshold(uint32_t threshold) {
    mmio_w32(plic_ctx_reg(PLIC_CTX_M_HART0, RVVM_PLIC_CTX_THRESHOLD),
             threshold & 0x7);
}

uint32_t plat_intc_claim(void) {
    return mmio_r32(plic_ctx_reg(PLIC_CTX_M_HART0, RVVM_PLIC_CTX_CLAIM));
}

void plat_intc_complete(uint32_t source) {
    mmio_w32(plic_ctx_reg(PLIC_CTX_M_HART0, RVVM_PLIC_CTX_CLAIM), source);
}

uint32_t plat_intc_source_limit(void) {
    return RVVM_PLIC_SRC_LIMIT;
}
