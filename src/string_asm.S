/* string.S — RV64 asm implementations of memset/memcpy/memmove.
 *
 * Replaces the C versions in string.c when HAL_ASM_STRING is set
 * (default on for HAL_PRIV=m, since the compiler-generated loops
 * spend ~60 % of their cycles on bookkeeping the unrolled asm
 * eliminates).
 *
 * All three operations follow the same shape:
 *   1. byte head until destination is 8-aligned
 *   2. main body unrolled 4× over 32-byte chunks (4 × sd or
 *      4 × ld+sd)
 *   3. byte/word tail for the residual <32 bytes
 * Memcpy and memmove gate the word path on src/dst sharing 8-bit
 * alignment (xor & 7 == 0); when they don't, the body falls through
 * to a byte loop. Real RV64 hardware traps on misaligned 8-byte
 * accesses unless Zicclsm is enabled; RVVM permits them but we still
 * keep this gate for portability.
 *
 * Memmove copies forward when dst < src (or dst == src; no-op),
 * backward when dst > src — to handle dst-into-src overlap.
 *
 * Naming: the public symbols (memcpy, memset, memmove) are emitted
 * with a single .globl each; the C build excludes string.c from the
 * archive when HAL_ASM_STRING is on (Makefile filter). All three live
 * in their own section (.text.<name>) so -ffunction-sections + -gc-
 * sections at link time can drop unused ones.
 *
 * Calling convention (RV64 psABI):
 *   memcpy/memmove:  a0 = dst, a1 = src, a2 = n;  returns dst (a0)
 *   memset:          a0 = dst, a1 = c,   a2 = n;  returns dst (a0)
 * No callee-saved registers touched — body uses t0..t6 only. */

    .option nopic

/* ====================================================================
 *  memset
 * ==================================================================== */
    .section .text.memset, "ax"
    .balign  4
    .globl   memset
    .type    memset, @function
memset:
    mv      t0, a0                  /* preserve return value */
    /* Splat byte c (a1) to all 8 lanes of t1. */
    andi    t1, a1, 0xff
    slli    t2, t1, 8
    or      t1, t1, t2
    slli    t2, t1, 16
    or      t1, t1, t2
    slli    t2, t1, 32
    or      t1, t1, t2

    /* Head: byte-by-byte until destination is 8-aligned. */
1:  beqz    a2, 9f
    andi    t2, t0, 7
    beqz    t2, 2f
    sb      a1, 0(t0)
    addi    t0, t0, 1
    addi    a2, a2, -1
    j       1b

    /* Body: 4× unrolled, 32 bytes per iteration. */
2:  li      t3, 32
    bltu    a2, t3, 4f
3:  sd      t1,  0(t0)
    sd      t1,  8(t0)
    sd      t1, 16(t0)
    sd      t1, 24(t0)
    addi    t0, t0, 32
    addi    a2, a2, -32
    bgeu    a2, t3, 3b

    /* Tail: 8 bytes at a time, then bytes. */
4:  li      t3, 8
    bltu    a2, t3, 6f
5:  sd      t1, 0(t0)
    addi    t0, t0, 8
    addi    a2, a2, -8
    bgeu    a2, t3, 5b
6:  beqz    a2, 9f
7:  sb      a1, 0(t0)
    addi    t0, t0, 1
    addi    a2, a2, -1
    bnez    a2, 7b
9:  ret
    .size   memset, . - memset

/* ====================================================================
 *  memcpy
 * ==================================================================== */
    .section .text.memcpy, "ax"
    .balign  4
    .globl   memcpy
    .type    memcpy, @function
memcpy:
    mv      t0, a0                  /* dst cursor (a0 stays for return) */
    mv      t1, a1                  /* src cursor */

    /* If src/dst don't share alignment, do a byte copy of the whole
     * thing — a misaligned 8-byte access traps on real RV64 unless
     * Zicclsm is enabled. (The full-misaligned-fast-path version
     * needs Zicclsm; keep it byte-loop here for portability.) */
    xor     t2, t0, t1
    andi    t2, t2, 7
    bnez    t2, .Lcpy_byteloop

    /* Head: byte-by-byte until 8-aligned. */
1:  beqz    a2, 9f
    andi    t2, t0, 7
    beqz    t2, 2f
    lb      t3, 0(t1)
    sb      t3, 0(t0)
    addi    t0, t0, 1
    addi    t1, t1, 1
    addi    a2, a2, -1
    j       1b

    /* Body: 4× unrolled, 32 bytes per iteration. */
2:  li      t6, 32
    bltu    a2, t6, 4f
3:  ld      t2,  0(t1)
    ld      t3,  8(t1)
    ld      t4, 16(t1)
    ld      t5, 24(t1)
    sd      t2,  0(t0)
    sd      t3,  8(t0)
    sd      t4, 16(t0)
    sd      t5, 24(t0)
    addi    t1, t1, 32
    addi    t0, t0, 32
    addi    a2, a2, -32
    bgeu    a2, t6, 3b

    /* Tail: 8 then 1. */
4:  li      t6, 8
    bltu    a2, t6, 6f
5:  ld      t2, 0(t1)
    sd      t2, 0(t0)
    addi    t1, t1, 8
    addi    t0, t0, 8
    addi    a2, a2, -8
    bgeu    a2, t6, 5b
6:  beqz    a2, 9f
.Lcpy_byteloop:
7:  beqz    a2, 9f
    lb      t2, 0(t1)
    sb      t2, 0(t0)
    addi    t1, t1, 1
    addi    t0, t0, 1
    addi    a2, a2, -1
    j       7b
9:  ret
    .size   memcpy, . - memcpy

/* ====================================================================
 *  memmove — forward if dst<src or no overlap, backward otherwise.
 *
 *  We don't try to detect "no overlap" — `memcpy` is fine for
 *  dst<src too. The backward-copy path triggers only when dst>src,
 *  which is the case where forward-copy would clobber unread source
 *  bytes.
 * ==================================================================== */
    .section .text.memmove, "ax"
    .balign  4
    .globl   memmove
    .type    memmove, @function
memmove:
    beq     a0, a1, .Lmove_done
    beqz    a2, .Lmove_done
    bltu    a0, a1, memcpy           /* dst<src: forward copy is safe */

    /* Backward path: t0=dst+n, t1=src+n; predecrement on each step. */
    add     t0, a0, a2
    add     t1, a1, a2

    xor     t2, t0, t1
    andi    t2, t2, 7
    bnez    t2, .Lmove_byteloop

    /* Head: byte-by-byte until t0 is 8-aligned. */
1:  beqz    a2, .Lmove_done
    andi    t2, t0, 7
    beqz    t2, 2f
    addi    t0, t0, -1
    addi    t1, t1, -1
    lb      t3, 0(t1)
    sb      t3, 0(t0)
    addi    a2, a2, -1
    j       1b

    /* Body: 4× unrolled backward, 32 bytes per iteration. */
2:  li      t6, 32
    bltu    a2, t6, 4f
3:  addi    t1, t1, -32
    addi    t0, t0, -32
    ld      t2, 24(t1)
    ld      t3, 16(t1)
    ld      t4,  8(t1)
    ld      t5,  0(t1)
    sd      t2, 24(t0)
    sd      t3, 16(t0)
    sd      t4,  8(t0)
    sd      t5,  0(t0)
    addi    a2, a2, -32
    bgeu    a2, t6, 3b

    /* Tail: 8 then 1. */
4:  li      t6, 8
    bltu    a2, t6, 6f
5:  addi    t1, t1, -8
    addi    t0, t0, -8
    ld      t2, 0(t1)
    sd      t2, 0(t0)
    addi    a2, a2, -8
    bgeu    a2, t6, 5b
6:  beqz    a2, .Lmove_done
.Lmove_byteloop:
7:  beqz    a2, .Lmove_done
    addi    t0, t0, -1
    addi    t1, t1, -1
    lb      t2, 0(t1)
    sb      t2, 0(t0)
    addi    a2, a2, -1
    j       7b
.Lmove_done:
    ret
    .size   memmove, . - memmove
