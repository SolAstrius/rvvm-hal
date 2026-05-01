/* trap.S — privilege-portable trap entry trampoline.
 *
 * Set as CSR_TVEC by irq_init. On entry the CPU has already copied
 * pc → CSR_EPC and prior IE → MPIE/SPIE before clearing global IE.
 *
 * Two paths share the same entry point:
 *
 *   slim path (interrupts) — saves only the registers the C ABI
 *     classifies as caller-saved + ra: ra, t0..t6, a0..a7. The rest
 *     (s0..s11, gp, tp) are callee-saved per psABI, so the C
 *     trap_dispatch preserves them across its execution. About 16
 *     register slots saved/restored vs the full 31, halving trap
 *     entry/exit memory traffic on the hot IRQ path (every UART byte,
 *     PLIC source, IPI, timer wake).
 *
 *   full path (synchronous exceptions) — saves all 31 GPRs in the
 *     classic per-register-number frame layout. The panic dumper in
 *     panic.c reads regs[1] (ra), regs[8] (s0/fp), regs[2] (entry sp),
 *     and prints the full set; sync exceptions never return, so this
 *     path is cold but layout-stable for tools/decode-panic.
 *
 * Path selection: cause register's MSB is the "interrupt" bit in
 * RV64. We branch on it after a single cheap save (slot 5 = t0)
 * lets us csrr the cause without losing a value.
 *
 * Slim-path frame slots actually written (slot index = register number):
 *   1 ra, 5 t0, 6 t1, 7 t2, 10..17 a0..a7, 28..31 t3..t6
 * Slot 2 (saved-sp) is also written so panic-from-IRQ-handler still
 * has a usable frame seed (panic.c reads regs[2] in its dumper). The
 * other slots remain undefined on entry to trap_dispatch — that's
 * fine; the IRQ dispatcher (handle_external etc.) doesn't read them.
 *
 * Aside on tp/gp: psABI says callee-preserves both. Our C compiler
 * abides; we don't save them on the slim path. If a later trap-time
 * tool ever needs the trapping code's tp/gp, it must use the full
 * path (e.g. by treating the trap as if it were a sync exception). */

#include "priv.h"

    .section .text
    .balign  4
    .global  __trap_entry
    .type    __trap_entry, @function
__trap_entry:
    addi    sp, sp, -32*8

    /* Save t0 first so we can clobber it for the cause check. */
    sd      t0,   5*8(sp)
    csrr    t0, CSR_CAUSE
    bltz    t0, .L_slim          /* MSB set → interrupt → slim path */

/* === Full path: synchronous exception, panic-bound. ============== */

    sd      x1,   1*8(sp)            /* ra */
    addi    x1, sp, 32*8
    sd      x1,   2*8(sp)            /* saved sp */
    sd      x3,   3*8(sp)            /* gp */
    sd      x4,   4*8(sp)            /* tp */
    /* x5 (t0) already saved */
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

    mv      a0, t0                   /* cause (already in t0) */
    csrr    a1, CSR_EPC
    mv      a2, sp
    call    trap_dispatch

    /* Sync exceptions don't return today (panic dumper wfi-loops),
     * but provide the restore for future use (ecall handlers etc.). */
    ld      x1,   1*8(sp)
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
    XRET

/* === Slim path: interrupt, ABI-aware (callee-saved skipped). === */

.L_slim:
    sd      x1,   1*8(sp)            /* ra */
    addi    x1, sp, 32*8
    sd      x1,   2*8(sp)            /* saved sp (for in-handler panic) */
    /* skip x3 gp, x4 tp — callee-saved per psABI */
    /* x5 (t0) already saved */
    sd      x6,   6*8(sp)            /* t1 */
    sd      x7,   7*8(sp)            /* t2 */
    /* skip x8 s0..x9 s1 — callee-saved */
    sd      x10, 10*8(sp)            /* a0 */
    sd      x11, 11*8(sp)            /* a1 */
    sd      x12, 12*8(sp)            /* a2 */
    sd      x13, 13*8(sp)            /* a3 */
    sd      x14, 14*8(sp)            /* a4 */
    sd      x15, 15*8(sp)            /* a5 */
    sd      x16, 16*8(sp)            /* a6 */
    sd      x17, 17*8(sp)            /* a7 */
    /* skip x18 s2..x27 s11 — callee-saved */
    sd      x28, 28*8(sp)            /* t3 */
    sd      x29, 29*8(sp)            /* t4 */
    sd      x30, 30*8(sp)            /* t5 */
    sd      x31, 31*8(sp)            /* t6 */

    mv      a0, t0                   /* cause */
    csrr    a1, CSR_EPC
    mv      a2, sp
    call    trap_dispatch

    ld      x1,   1*8(sp)
    ld      x5,   5*8(sp)
    ld      x6,   6*8(sp)
    ld      x7,   7*8(sp)
    ld      x10, 10*8(sp)
    ld      x11, 11*8(sp)
    ld      x12, 12*8(sp)
    ld      x13, 13*8(sp)
    ld      x14, 14*8(sp)
    ld      x15, 15*8(sp)
    ld      x16, 16*8(sp)
    ld      x17, 17*8(sp)
    ld      x28, 28*8(sp)
    ld      x29, 29*8(sp)
    ld      x30, 30*8(sp)
    ld      x31, 31*8(sp)
    addi    sp, sp, 32*8
    XRET

    .size   __trap_entry, . - __trap_entry
