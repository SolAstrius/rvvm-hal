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
     * Standard RISC-V boot ABI, set in src/rvvm.c:312-316. */

    /* Park any non-zero hart in a wfi loop — we run single-hart only. */
    csrr    t0, mhartid
    bnez    t0, park

    /* Save a0/a1 across the BSS-zeroing loop so kmain receives them. */
    mv      s0, a0
    mv      s1, a1

    /* Set up stack — top of the reserved region in link.ld. */
    la      sp, __stack_top

    /* Zero BSS. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:

    /* Restore boot args, jump to kmain(hartid, fdt_ptr). */
    mv      a0, s0
    mv      a1, s1
    call    kmain

    /* If kmain returns, halt. */
park:
    wfi
    j       park

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
