/* Entry point. mtd-physmap drops us at 0x80000000 with no stack, no zero'd
 * bss, no nothing. Set sp, zero bss, jump to kmain.
 *
 * Privilege: this file is the M-mode entry path. Built only when the
 * HAL is configured for M-mode (HAL_PRIV_M, the default). The S-mode
 * port has its own start_s.S that takes the SBI-handoff ABI (we're
 * dropped at the configured payload address with a0=hartid, a1=fdt
 * already set up, no need to pull from CSRs, no MSIP-park dance —
 * sbi_hart_start brings secondaries up directly). priv.h is included
 * for the CSR aliases the C code uses; this file uses the M-mode
 * mnemonics directly because the boot path itself is mode-specific. */

#include "priv.h"

#ifdef HAL_PRIV_S
#  error "start.S is the M-mode boot path; build with HAL_PRIV=m or use start_s.S"
#endif

    .section .text.boot, "ax"
    .global _start
    .extern __bss_start
    .extern __bss_end
    .extern __stack_top
    .extern kmain

_start:
    /* RVVM hands us:
     *   a0 = mhartid
     *   a1 = physical address of the FDT (Flattened Device Tree)
     *   PC = reset_pc (= RAM base by default)
     *   M-mode, MIE clear.
     * Standard RISC-V boot ABI, set in src/rvvm.c:312-316.
     *
     * All harts enter here simultaneously. We split into two paths
     * by mhartid:
     *   hart 0           — primary: zero BSS, call kmain.
     *   hart 1..MAX-1    — secondary: park in a wfi loop, dispatch on
     *                      MSIP wake. Stack is `__stack_top - hartid * 16K`.
     *   hart MAX..       — out of bounds: park forever, no stack.
     * MAX must match SMP_MAX_HARTS in include/smp.h. */

    csrr    s0, mhartid

#ifdef HAL_NO_SMP
    /* Single-hart build: any non-zero hart parks immediately without
     * a stack. The link.ld layout still reserves SMP_MAX_HARTS slots
     * (it has no way to read the C macro), so address space is
     * unchanged — only behaviour differs. */
    bnez    s0, park_oob
    la      sp, __stack_top
#else
    /* Out-of-bounds harts: skip stack setup and park forever. We
     * can't allocate a stack for them (link.ld only reserves
     * SMP_MAX_HARTS slots) and they have no work to do. */
    li      t0, 8                 /* SMP_MAX_HARTS — keep in sync with smp.h */
    bgeu    s0, t0, park_oob

    /* Per-hart stack: sp = __stack_top - mhartid * 16384.
     * link.ld reserves SMP_MAX_HARTS slots of SMP_STACK_PER_HART
     * each, with __stack_top at the high end. Hart 0 gets the
     * topmost slot. */
    li      t0, 16384             /* SMP_STACK_PER_HART */
    mul     t0, s0, t0
    la      sp, __stack_top
    sub     sp, sp, t0
#endif

    /* Enable the FP context. mstatus.FS is in bits 14:13; reset
     * value is 00 (Off), which makes every F/D instruction trap
     * with "Illegal Instruction". Set FS = 01 (Initial) so the
     * F/D extensions can be used by C code that the compiler
     * decides to emit FP instructions in (printf with %f, math
     * libraries, etc.).
     *
     * This is independent of mstatus.MIE — interrupts stay off
     * until irq_global_enable(). RVVM advertises the F+D
     * extensions in misa, so the FP instructions decode correctly
     * once FS != Off. */
    li      t0, 0x2000            /* mstatus.FS = 01 (Initial) */
    csrs    mstatus, t0

    /* Save FDT (a1) somewhere callee-save survives our setup.
     * Only the primary uses it but the cost is one register. */
    mv      s1, a1

#ifndef HAL_NO_SMP
    bnez    s0, secondary_park
#endif

    /* === Primary === */

    /* Zero BSS. Secondaries don't touch BSS during their park loop
     * (only MMIO and CSRs), so the race window is harmless: they
     * all just spin in wfi until primary's smp_start writes their
     * command slot, which by definition happens after kmain begins
     * and therefore after BSS zero completes. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:

    /* Set mtvec to our trampoline early so primary panics get a
     * register dump even before irq_init wires the PLIC. */
    la      t0, __trap_entry
    csrw    mtvec, t0

    mv      a0, s0
    mv      a1, s1
    call    kmain
    j       park_oob

    /* === Secondary park loop ===
     *
     * Wait for the primary to call smp_start (which writes our
     * command slot and pulses CLINT msip). MSIP-pending wakes us
     * from wfi without trapping because mstatus.MIE stays off —
     * we just observe the pending bit, clear it, dispatch.
     *
     * We point our own mtvec at __trap_entry so synchronous
     * exceptions inside the dispatched function panic via the same
     * path as primary, instead of jumping into vector zero. */
#ifndef HAL_NO_SMP
secondary_park:
    la      t0, __trap_entry
    csrw    mtvec, t0

    /* Enable mie.MSIE. mstatus.MIE stays off so MSIP-pending
     * wakes wfi but does not trap — the park loop reads msip
     * directly and dispatches. */
    li      t0, 1 << 3            /* MIE_MSIE */
    csrs    mie, t0

    /* Pre-compute msip[hartid] = 0x02000000 + hartid*4. Hardcoded
     * to RVVM's default CLINT base — primary's smp.c uses the
     * FDT-discovered base for all subsequent IPIs, but this very
     * first wake-up uses the default. RVVM never moves CLINT in
     * the default machine, so this is reliable. */
    li      s2, 0x02000000        /* RVVM_CLINT_BASE */
    slli    t1, s0, 2
    add     s2, s2, t1            /* s2 = &msip[mhartid] */

1:  wfi
    lw      t0, 0(s2)
    beqz    t0, 1b                /* spurious wake — back to wfi */
    sw      zero, 0(s2)           /* clear pending */

    mv      a0, s0
    call    __smp_secondary_dispatch
    j       1b
#endif /* !HAL_NO_SMP */

    /* Out-of-bounds harts and primary kmain-return path land here. */
park_oob:
    wfi
    j       park_oob

/* __trap_entry lives in trap.S — privilege-portable, shared with the
 * S-mode boot path. */
