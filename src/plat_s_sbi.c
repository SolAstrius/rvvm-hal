/* plat_s_sbi.c — S-mode payload under OpenSBI.
 *
 * Built when HAL_PRIV=s HAL_PLAT=s_sbi. Requires an SBI v0.2+ firmware
 * underneath us (OpenSBI ≥ 0.5 satisfies this). The HAL stops poking
 * CLINT directly: timer and IPI are SBI ecalls; secondary harts are
 * brought up by sbi_hart_start (HSM extension) instead of MSIP-park;
 * PLIC is still PLIC but accessed at the S-mode context.
 *
 * Privilege boundaries:
 *   - mstatus / mtvec / mret are illegal in S-mode. Anything that
 *     used to touch them now goes through priv.h aliases (sstatus /
 *     stvec / sret) or, for things that have no S-mode CSR (CLINT
 *     msip, mtimecmp), through SBI ecalls.
 *   - mhartid is M-only. Hartid reaches us in a0 from SBI at boot;
 *     start_s.S is responsible for stashing it in tp before any C
 *     code runs. plat_this_hart reads tp.
 *
 * SBI calling convention:
 *     a7 = EID  (extension ID)
 *     a6 = FID  (function ID)
 *     a0..a5    arguments  (a0 returns error, a1 returns value)
 *     ecall
 * Errors are negative SBI_ERR_* codes; a successful call sets a0=0.
 * We don't propagate errors from these wrappers — the operations they
 * abstract (set timer, send IPI, mask an IRQ) are infallible at the
 * HAL's level of concern, and any genuine SBI failure here means the
 * firmware contract is broken in a way the HAL can't recover from. */

#include "plat.h"
#include "priv.h"
#include "rvvm.h"
#include "mmio.h"
#include <stddef.h>

/* SBI extension IDs (SBI v0.2+ specification, "EID" column). */
#define SBI_EID_TIME   0x54494D45UL   /* "TIME" — sbi_set_timer */
#define SBI_EID_IPI    0x735049UL     /* "sPI"  — sbi_send_ipi  */
#define SBI_EID_HSM    0x48534DUL     /* "HSM"  — hart-state mgmt */

#define SBI_FID_SET_TIMER     0
#define SBI_FID_SEND_IPI      0
#define SBI_FID_HART_START    0

/* The S-mode build talks to PLIC at the S-mode context for the
 * calling hart: ctx = (hartid << 1) | 1. RVVM's PLIC implementation
 * at riscv-plic.c:43-54 honours this convention. */
static uintptr_t plic_base = RVVM_PLIC_BASE;

/* Sstc fast-path. Set by plat_probe_features when the FDT advertises
 * Sstc on the cpu nodes; if true, plat_timer_set_deadline writes
 * stimecmp directly instead of ecalling sbi_set_timer. Saves the
 * ecall round-trip on every frame deadline (~50 µs measured under
 * RVVM+OpenSBI 1.4 on a stock laptop). Default false: fall back to
 * the ecall path which is universally supported. */
static bool use_sstc = false;

/* ====================================================================
 *  SBI ecall trampolines
 *
 *  Two-argument variant covers everything we currently use; HSM
 *  hart_start needs three. Argument-register width is XLEN — `unsigned
 *  long` is the right type on RV64. set_timer takes a 64-bit value
 *  which on RV64 fits in a single argument register; on RV32 SBI it'd
 *  need two — we only target RV64.
 * ==================================================================== */

struct sbiret { long error; long value; };

static inline struct sbiret sbi_call2(unsigned long eid, unsigned long fid,
                                      unsigned long a0, unsigned long a1) {
    register unsigned long r_a0 __asm__("a0") = a0;
    register unsigned long r_a1 __asm__("a1") = a1;
    register unsigned long r_a6 __asm__("a6") = fid;
    register unsigned long r_a7 __asm__("a7") = eid;
    __asm__ volatile ("ecall"
                      : "+r"(r_a0), "+r"(r_a1)
                      : "r"(r_a6), "r"(r_a7)
                      : "memory");
    return (struct sbiret){ .error = (long)r_a0, .value = (long)r_a1 };
}

static inline struct sbiret sbi_call3(unsigned long eid, unsigned long fid,
                                      unsigned long a0, unsigned long a1,
                                      unsigned long a2) {
    register unsigned long r_a0 __asm__("a0") = a0;
    register unsigned long r_a1 __asm__("a1") = a1;
    register unsigned long r_a2 __asm__("a2") = a2;
    register unsigned long r_a6 __asm__("a6") = fid;
    register unsigned long r_a7 __asm__("a7") = eid;
    __asm__ volatile ("ecall"
                      : "+r"(r_a0), "+r"(r_a1)
                      : "r"(r_a2), "r"(r_a6), "r"(r_a7)
                      : "memory");
    return (struct sbiret){ .error = (long)r_a0, .value = (long)r_a1 };
}

/* ====================================================================
 *  Lifecycle
 * ==================================================================== */
void plat_init(uintptr_t intc_base, uintptr_t clint) {
    if (intc_base) plic_base = intc_base;
    /* CLINT base is meaningless to us — SBI mediates everything that
     * would otherwise touch CLINT MMIO. Accept the argument for
     * interface parity with plat_m_clint. */
    (void)clint;
}

uint32_t plat_this_hart(void) {
    /* Hartid lives in tp by convention. start_s.S stashes the SBI-
     * provided a0 into tp before kmain runs; secondaries do the same
     * in their sbi_hart_start entry trampoline. csrr mhartid would
     * trap in S-mode. */
    uint64_t v;
    __asm__ volatile ("mv %0, tp" : "=r"(v));
    return (uint32_t)v;
}

/* ====================================================================
 *  IPI via sbi_send_ipi(hart_mask, hart_mask_base)
 *
 *  The mask is interpreted relative to hart_mask_base: bit N of the
 *  mask targets hart (base + N). For point-to-point delivery we set
 *  base = hartid and mask = 1.
 * ==================================================================== */
void plat_ipi_send(uint32_t hartid) {
    /* Same fence semantics as plat_m_clint — callers expect prior
     * stores to be visible to the target hart before it observes the
     * IPI. ecall already implies a serialising boundary in OpenSBI's
     * implementation, but the explicit fence is cheap and documents
     * the contract. */
    __asm__ volatile ("fence rw,rw" ::: "memory");
    sbi_call2(SBI_EID_IPI, SBI_FID_SEND_IPI, /*mask=*/1, /*base=*/hartid);
}

void plat_ipi_ack(uint32_t self_hartid) {
    (void)self_hartid;
    /* Clear the supervisor software-interrupt-pending bit on this
     * hart. In M-mode the equivalent is "write 0 to msip[hartid]";
     * in S-mode SSIP is software-writable directly via csrc sip. */
    uintptr_t bit = IE_SOFT_BIT;   /* sip.SSIP = bit 1 */
    __asm__ volatile ("csrc " HAL_CSR(CSR_IP) ", %0" :: "r"(bit));
}

/* ====================================================================
 *  Timer via sbi_set_timer(stime_value)
 *
 *  SBI guarantees that setting a deadline in the past pends the timer
 *  immediately, and that a single ecall replaces any prior deadline
 *  (no need to clear separately). Pass UINT64_MAX to disarm.
 *
 *  Note: the Sstc extension exposes stimecmp as an S-mode-writable
 *  CSR, eliminating the ecall on each frame. We could probe misa /
 *  the FDT and skip SBI when Sstc is present — keeping it simple
 *  for now since OpenSBI's SBI_EID_TIME path is universally available.
 * ==================================================================== */
void plat_timer_set_deadline(uint64_t deadline) {
    if (use_sstc) {
        /* Direct stimecmp write — no ecall, no privilege transition.
         * Spec: writing stimecmp clears any prior pending sip.STIP and
         * arms a new deadline; no separate clear needed. Available
         * under the Sstc extension (riscv,isa-extensions contains
         * "sstc" on each cpu node). */
        __asm__ volatile ("csrw stimecmp, %0" :: "r"(deadline));
        return;
    }
    sbi_call2(SBI_EID_TIME, SBI_FID_SET_TIMER, deadline, 0);
}

void plat_timer_irq_enable(void) {
    uintptr_t bit = IE_TIMER_BIT;   /* sie.STIE */
    __asm__ volatile ("csrs " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

void plat_timer_irq_disable(void) {
    uintptr_t bit = IE_TIMER_BIT;
    __asm__ volatile ("csrc " HAL_CSR(CSR_IE) ", %0" :: "r"(bit));
}

/* ====================================================================
 *  Interrupt controller — SiFive PLIC at the S-mode context
 *
 *  Same MMIO layout as the M-mode build; only the context index
 *  changes. RVVM models both M and S contexts per hart, so the same
 *  PLIC base address works.
 * ==================================================================== */
static inline uint32_t plic_ctx_self(void) {
    /* (hartid << 1) | 1  — see riscv-plic.c:43-54 */
    return (plat_this_hart() << 1) | 1u;
}

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
    uintptr_t reg = plic_enable_reg(plic_ctx_self(), source >> 5);
    uint32_t  cur = mmio_r32(reg);
    uint32_t  bit = 1u << (source & 0x1F);
    mmio_w32(reg, enable ? (cur | bit) : (cur & ~bit));
}

void plat_intc_init(void) {
    uint32_t ctx = plic_ctx_self();
    for (uint32_t word = 0; word < (RVVM_PLIC_SRC_LIMIT + 31) / 32; word++) {
        mmio_w32(plic_enable_reg(ctx, word), 0);
    }
    /* Source priorities are global (not per-context), so only the
     * primary should zero them — but doing it on every hart is
     * harmless: the values are already 0 after the first pass and
     * the PLIC accepts redundant writes. */
    for (uint32_t i = 1; i < RVVM_PLIC_SRC_LIMIT; i++) {
        mmio_w32(plic_prio_reg(i), 0);
    }
    mmio_w32(plic_ctx_reg(ctx, RVVM_PLIC_CTX_THRESHOLD), 0);
}

void plat_intc_enable_source(uint32_t source) {
    if (source == 0 || source >= RVVM_PLIC_SRC_LIMIT) return;
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
    mmio_w32(plic_ctx_reg(plic_ctx_self(), RVVM_PLIC_CTX_THRESHOLD),
             threshold & 0x7);
}

uint32_t plat_intc_claim(void) {
    return mmio_r32(plic_ctx_reg(plic_ctx_self(), RVVM_PLIC_CTX_CLAIM));
}

void plat_intc_complete(uint32_t source) {
    mmio_w32(plic_ctx_reg(plic_ctx_self(), RVVM_PLIC_CTX_CLAIM), source);
}

uint32_t plat_intc_source_limit(void) {
    return RVVM_PLIC_SRC_LIMIT;
}

/* ====================================================================
 *  Runtime feature probe
 *
 *  Walks the FDT for the boot hart's `riscv,isa-extensions` property
 *  (a NUL-separated stringlist; each entry is a lowercase extension
 *  name like "sstc", "zicntr", "svpbmt"). If "sstc" is present we
 *  switch plat_timer_set_deadline to the direct-CSR fast path.
 *
 *  We probe only the first cpu node — RV64 conventions require all
 *  harts in a single image to advertise the same extension set, and
 *  RVVM in particular emits identical extension lists on every hart.
 *  If a heterogeneous machine ever needs per-hart probing, this is
 *  where to add it. */
static bool stringlist_contains(const char *list, uint32_t list_len,
                                const char *needle) {
    /* Each entry is NUL-terminated; total bytes = list_len. We march
     * through the buffer comparing each entry to needle. */
    uint32_t pos = 0;
    while (pos < list_len) {
        const char *entry = list + pos;
        uint32_t entry_len = 0;
        while (pos + entry_len < list_len && entry[entry_len] != 0) {
            entry_len++;
        }
        /* compare needle against this entry */
        uint32_t i = 0;
        while (entry[i] && needle[i] && entry[i] == needle[i]) i++;
        if (entry[i] == 0 && needle[i] == 0) return true;
        pos += entry_len + 1;        /* skip past terminating NUL */
    }
    return false;
}

void plat_probe_features(const fdt_t *fdt) {
    if (!fdt) return;
    uint32_t cpu_off = fdt_find_node_named(fdt, "cpu");
    if (cpu_off == UINT32_MAX) return;

    uint32_t len = 0;
    const char *exts = fdt_node_prop(fdt, cpu_off,
                                     "riscv,isa-extensions", &len);
    if (!exts || len == 0) {
        /* RVVM emits riscv,isa-extensions on every cpu node. Older
         * machines that only emit `riscv,isa` (single concatenated
         * string with underscores between groups) aren't probed here
         * — fall back to the ecall path, which works universally. */
        use_sstc = false;
        return;
    }

    use_sstc = stringlist_contains(exts, len, "sstc");

#ifdef HAL_NO_SSTC
    /* Build-time override: force the SBI ecall path even if Sstc is
     * available. Used for A/B comparison of the two timer paths;
     * never set in normal builds. */
    use_sstc = false;
#endif
}

/* ====================================================================
 *  Hart bring-up via SBI HSM
 *
 *  Under SBI, secondaries are STOPPED at boot — only the calling hart
 *  enters our payload. plat_hart_bringup_park brings each secondary
 *  up at start_s.S's _smp_secondary_entry, which sets up its stack +
 *  trap vector then parks waiting for IPI (sip.SSIP). The opaque
 *  argument is unused — the entry recovers hartid from SBI's a0.
 *
 *  smp.c calls this once per non-primary hart from smp_init, before
 *  any caller touches smp_start. After return, the secondary is in
 *  the park loop and reachable via plat_ipi_send. */
extern void _smp_secondary_entry(void);   /* start_s.S */

void plat_hart_bringup_park(uint32_t hartid) {
    sbi_call3(SBI_EID_HSM, SBI_FID_HART_START,
              hartid,
              (unsigned long)&_smp_secondary_entry,
              /*opaque=*/0);
}
