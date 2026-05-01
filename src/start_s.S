/* start_s.S — S-mode entry path under SBI.
 *
 * Two entry points:
 *   _start                  cold-boot. SBI hands off here on the boot
 *                           hart with a0=hartid, a1=fdt_phys. We're in
 *                           S-mode, MMU off (OpenSBI default fw_jump),
 *                           sp/gp/tp undefined. Sets up sp + tp, zeros
 *                           BSS, points stvec at the trap trampoline,
 *                           enables sstatus.FS, calls kmain.
 *
 *   _smp_secondary_entry    target of sbi_hart_start (HSM extension)
 *                           when smp_init brings secondaries up.
 *                           Same ABI as _start (a0=hartid, a1=opaque
 *                           — we ignore opaque). Sets up its own sp +
 *                           tp + stvec, enables sie.SSIE so wfi wakes
 *                           on IPI, parks in a sip.SSIP poll loop and
 *                           dispatches via __smp_secondary_dispatch.
 *
 * Differences from start.S:
 *   - mhartid is illegal in S-mode; hartid arrives in a0 from SBI.
 *     We stash it in tp so plat_this_hart() can read it later.
 *   - No MSIP poke for secondary wake — sbi_hart_start delivers them
 *     directly to _smp_secondary_entry. The IPI mechanism for runtime
 *     dispatch is sbi_send_ipi (handled by plat_s_sbi.c).
 *   - mret → sret (via priv.h's XRET — though this file doesn't return
 *     from a trap, only normal calls).
 *   - mstatus → sstatus, mtvec → stvec, mie → sie, mip → sip — via
 *     priv.h aliases (CSR_STATUS / CSR_TVEC / CSR_IE / CSR_IP).
 *
 * link.ld layout reused unchanged: __stack_top reserves SMP_MAX_HARTS
 * slots of 16 KiB each, with the topmost slot for the boot hart.
 * Whatever hartid SBI hands us, the stack math (top - hartid * 16 KiB)
 * still picks a unique slot per hart. Consumer firmwares targeting
 * OpenSBI must adjust their link.ld base address (default 0x80000000
 * collides with OpenSBI itself; OpenSBI's fw_jump payload typically
 * lives at 0x80200000). That's a per-firmware concern, not ours. */

#include "priv.h"

#ifdef HAL_PRIV_M
#  error "start_s.S is the S-mode boot path; build with HAL_PRIV=s or use start.S"
#endif

    .section .text.boot, "ax"
    .global _start
    .extern __bss_start
    .extern __bss_end
    .extern __stack_top
    .extern kmain
    .extern __trap_entry

_start:
    /* SBI handoff:  a0 = hartid, a1 = fdt_phys.
     * Stash hartid in tp (the S-mode equivalent of mhartid for the
     * rest of the HAL — plat_s_sbi.c's plat_this_hart reads it). */
    mv      tp, a0
    mv      s0, a0          /* preserve hartid across stack setup */
    mv      s1, a1          /* preserve fdt across stack setup */

    /* Per-hart stack: sp = __stack_top - hartid * 16384.
     * Same layout as start.S; the boot hart need not be hart 0 under
     * SBI in principle, though RVVM + OpenSBI conventionally use 0. */
    li      t0, 16384
    mul     t0, s0, t0
    la      sp, __stack_top
    sub     sp, sp, t0

    /* Enable FP context: sstatus.FS = 01 (Initial), bits 14:13 (same
     * positions as mstatus.FS). Without this, F/D instructions trap. */
    li      t0, 0x2000
    csrs    CSR_STATUS, t0

    /* Trap vector: direct mode (low 2 bits = 0). Set early so a fault
     * during BSS zero or kmain prologue produces a register dump via
     * the normal panic path instead of jumping to virtual address 0. */
    la      t0, __trap_entry
    csrw    CSR_TVEC, t0

    /* Zero BSS. Only the boot hart runs this — secondaries land at
     * _smp_secondary_entry, which skips BSS entirely. */
    la      t0, __bss_start
    la      t1, __bss_end
1:  bgeu    t0, t1, 2f
    sd      zero, 0(t0)
    addi    t0, t0, 8
    j       1b
2:

    mv      a0, s0          /* hartid */
    mv      a1, s1          /* fdt    */
    call    kmain

    /* kmain returned (firmware finished or fell through). Park. */
    j       park_oob

#ifndef HAL_NO_SMP
/* ====================================================================
 *  _smp_secondary_entry
 *
 *  Reached when plat_hart_bringup_park (smp_init) called
 *  sbi_hart_start(h, &_smp_secondary_entry, 0). SBI lands us here
 *  with a0 = hartid, a1 = opaque (= 0, ignored).
 *
 *  We set up our stack and trap vector, enable sie.SSIE so wfi wakes
 *  on incoming IPI, then loop: wait for sip.SSIP, clear it, dispatch
 *  via the smp_cmd[] slot, repeat. The dispatch path is identical to
 *  the M+CLINT secondary loop — only the wake mechanism differs.
 * ==================================================================== */
    .global _smp_secondary_entry
    .type   _smp_secondary_entry, @function
_smp_secondary_entry:
    mv      tp, a0
    mv      s0, a0

    li      t0, 16384
    mul     t0, s0, t0
    la      sp, __stack_top
    sub     sp, sp, t0

    li      t0, 0x2000
    csrs    CSR_STATUS, t0

    la      t0, __trap_entry
    csrw    CSR_TVEC, t0

    /* Enable SSIE so a pending SSIP wakes wfi. sstatus.SIE stays off
     * — we don't want IPIs to trap; we observe the pending bit
     * directly and dispatch from the park loop, mirroring the M+CLINT
     * MSIE-without-MIE arrangement. */
    li      t0, IE_SOFT_BIT
    csrs    CSR_IE, t0

1:  wfi
    csrr    t0, CSR_IP
    andi    t0, t0, IE_SOFT_BIT
    beqz    t0, 1b           /* spurious wake — back to wfi */

    /* Clear SSIP. sip.SSIP is software-writable from S-mode; in
     * M+CLINT the equivalent is "write 0 to msip[hartid]". */
    li      t0, IE_SOFT_BIT
    csrc    CSR_IP, t0

    mv      a0, s0
    call    __smp_secondary_dispatch
    j       1b

    .size   _smp_secondary_entry, . - _smp_secondary_entry
#endif /* !HAL_NO_SMP */

park_oob:
    wfi
    j       park_oob
