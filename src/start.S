/* Entry point. mtd-physmap drops us at 0x80000000 with no stack, no zero'd
 * bss, no nothing. Set sp, zero bss, jump to kmain. */

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

/* ====================================================================
 *  Trap entry. Set as mtvec by irq_init() (direct mode, low 2 bits 00).
 *
 *  When a trap fires, hardware switches to M-mode (we're already in M),
 *  copies pc → mepc, copies the prior MIE → MPIE and clears MIE. We
 *  save callee-clobbered + caller-saved + s/t/a/gp/tp into a 32*8-byte
 *  frame on the current stack, hand the C dispatcher a pointer to it
 *  alongside mcause+mepc, then restore + mret.
 *
 *  Stack invariants:
 *  - We use the kernel stack the trapped code was already on. RVVM
 *    only ever runs us in M-mode, and our stack has 64 KiB of slack
 *    (link.ld), so 256 B per nested trap is safe even if trap_dispatch
 *    re-enables interrupts (which today it doesn't).
 *  - x0 is hardwired zero (skipped). x2 (sp) is preserved implicitly:
 *    we save the post-decrement value to slot 2 and reverse on exit.
 *
 *  Frame layout (slot index = register number for x1..x31):
 *      0*8(sp)  ra (x1)
 *      1*8(sp)  -- (x0 placeholder; never read or written)
 *      2*8(sp)  saved-sp = entry sp (caller stack pointer pre-decrement)
 *      3*8(sp)  gp (x3)
 *      4*8(sp)  tp (x4)
 *      5*8(sp)  t0 (x5)        ... etc through x31
 *  ==================================================================== */

    .section .text
    .balign  4
    .global  __trap_entry
    .type    __trap_entry, @function
__trap_entry:
    addi    sp, sp, -32*8

    sd      x1,   1*8(sp)            /* ra */
    /* x2 (sp): save the entry value (post-decrement + 32*8 = pre-decrement). */
    addi    x1, sp, 32*8
    sd      x1,   2*8(sp)
    sd      x3,   3*8(sp)            /* gp */
    sd      x4,   4*8(sp)            /* tp */
    sd      x5,   5*8(sp)            /* t0 */
    sd      x6,   6*8(sp)            /* t1 */
    sd      x7,   7*8(sp)            /* t2 */
    sd      x8,   8*8(sp)            /* s0 */
    sd      x9,   9*8(sp)            /* s1 */
    sd      x10, 10*8(sp)            /* a0 */
    sd      x11, 11*8(sp)            /* a1 */
    sd      x12, 12*8(sp)            /* a2 */
    sd      x13, 13*8(sp)            /* a3 */
    sd      x14, 14*8(sp)            /* a4 */
    sd      x15, 15*8(sp)            /* a5 */
    sd      x16, 16*8(sp)            /* a6 */
    sd      x17, 17*8(sp)            /* a7 */
    sd      x18, 18*8(sp)            /* s2 */
    sd      x19, 19*8(sp)            /* s3 */
    sd      x20, 20*8(sp)            /* s4 */
    sd      x21, 21*8(sp)            /* s5 */
    sd      x22, 22*8(sp)            /* s6 */
    sd      x23, 23*8(sp)            /* s7 */
    sd      x24, 24*8(sp)            /* s8 */
    sd      x25, 25*8(sp)            /* s9 */
    sd      x26, 26*8(sp)            /* s10 */
    sd      x27, 27*8(sp)            /* s11 */
    sd      x28, 28*8(sp)            /* t3 */
    sd      x29, 29*8(sp)            /* t4 */
    sd      x30, 30*8(sp)            /* t5 */
    sd      x31, 31*8(sp)            /* t6 */

    /* trap_dispatch(uint64_t mcause, uint64_t mepc, uint64_t *regs) */
    csrr    a0, mcause
    csrr    a1, mepc
    mv      a2, sp
    call    trap_dispatch

    ld      x1,   1*8(sp)
    /* x2 reload skipped — sp must remain pointing at the frame until
     * the final addi; entry sp is recovered by addi sp, sp, 32*8. */
    ld      x3,   3*8(sp)
    ld      x4,   4*8(sp)
    ld      x5,   5*8(sp)
    ld      x6,   6*8(sp)
    ld      x7,   7*8(sp)
    ld      x8,   8*8(sp)
    ld      x9,   9*8(sp)
    ld      x10, 10*8(sp)
    ld      x11, 11*8(sp)
    ld      x12, 12*8(sp)
    ld      x13, 13*8(sp)
    ld      x14, 14*8(sp)
    ld      x15, 15*8(sp)
    ld      x16, 16*8(sp)
    ld      x17, 17*8(sp)
    ld      x18, 18*8(sp)
    ld      x19, 19*8(sp)
    ld      x20, 20*8(sp)
    ld      x21, 21*8(sp)
    ld      x22, 22*8(sp)
    ld      x23, 23*8(sp)
    ld      x24, 24*8(sp)
    ld      x25, 25*8(sp)
    ld      x26, 26*8(sp)
    ld      x27, 27*8(sp)
    ld      x28, 28*8(sp)
    ld      x29, 29*8(sp)
    ld      x30, 30*8(sp)
    ld      x31, 31*8(sp)

    addi    sp, sp, 32*8
    mret

    .size   __trap_entry, . - __trap_entry
