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
