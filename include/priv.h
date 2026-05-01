/* priv.h — privilege-mode CSR aliases.
 *
 * The HAL was originally written M-mode-only. To allow the same source
 * to also run as an S-mode payload under SBI (and later, with smaller
 * deltas, on hypervisor VS-mode or similar), every privileged CSR
 * access goes through a name that resolves to the right CSR for the
 * build's privilege level.
 *
 * Selection is at compile time via the Makefile's HAL_PRIV variable:
 *   HAL_PRIV=m   (default) → -DHAL_PRIV_M=1   → mstatus / mie / mtvec / mret …
 *   HAL_PRIV=s            → -DHAL_PRIV_S=1   → sstatus / sie / stvec / sret …
 *
 * This header is included by both .c (for inline asm) and .S files
 * (for direct csrr/csrw mnemonics). Bare register names work in both:
 *   - In C inline asm, use `HAL_CSR(CSR_STATUS)` to stringify into the
 *     asm template (`"csrr %0, " HAL_CSR(CSR_STATUS)`).
 *   - In .S, write `csrr t0, CSR_STATUS` directly — gcc preprocesses
 *     `.S` files (it's `.s` that skips cpp), so the macro expands
 *     before the assembler sees it.
 *
 * Bit positions (RV64):
 *   - mstatus.FS / sstatus.FS       both at bits 14:13.
 *   - mstatus.MIE = bit 3;          sstatus.SIE = bit 1.
 *   - mie/mip M-bits (ext/timer/sw) at bits 11/7/3.
 *   - mie/mip S-bits (ext/timer/sw) at bits  9/5/1.
 *   - mcause/scause "interrupt" bit at XLEN-1 (= 63 on RV64) in either.
 *
 * Trap return:
 *   - M-mode: `mret`.  S-mode: `sret`.  Use the XRET macro.
 *
 * What this header does NOT cover:
 *   - mhartid is M-mode-only. S-mode firmware receives hartid in `a0`
 *     from SBI at boot and is expected to stash it (conventionally in
 *     `tp`); use plat_this_hart() instead of csrr mhartid.
 *   - mtval2 / mtinst / hypervisor CSRs aren't aliased — neither path
 *     touches them today.
 *   - SBI ecalls live in plat_s_sbi.c, not here. */

#pragma once

#if defined(HAL_PRIV_S)

#  define CSR_STATUS    sstatus
#  define CSR_IE        sie
#  define CSR_IP        sip
#  define CSR_TVEC      stvec
#  define CSR_EPC       sepc
#  define CSR_CAUSE     scause
#  define CSR_TVAL      stval
#  define CSR_SCRATCH   sscratch
#  define XRET          sret

#  define STATUS_IE_BIT (1ULL << 1)   /* sstatus.SIE */
#  define IE_EXT_BIT    (1ULL << 9)   /* sie.SEIE */
#  define IE_TIMER_BIT  (1ULL << 5)   /* sie.STIE */
#  define IE_SOFT_BIT   (1ULL << 1)   /* sie.SSIE */

#  define CAUSE_EXTERNAL  9           /* supervisor external interrupt */
#  define CAUSE_TIMER     5           /* supervisor timer interrupt */
#  define CAUSE_SOFTWARE  1           /* supervisor software interrupt */

#else /* default: M-mode */

#  ifndef HAL_PRIV_M
#    define HAL_PRIV_M 1
#  endif

#  define CSR_STATUS    mstatus
#  define CSR_IE        mie
#  define CSR_IP        mip
#  define CSR_TVEC      mtvec
#  define CSR_EPC       mepc
#  define CSR_CAUSE     mcause
#  define CSR_TVAL      mtval
#  define CSR_SCRATCH   mscratch
#  define XRET          mret

#  define STATUS_IE_BIT (1ULL << 3)   /* mstatus.MIE */
#  define IE_EXT_BIT    (1ULL << 11)  /* mie.MEIE */
#  define IE_TIMER_BIT  (1ULL << 7)   /* mie.MTIE */
#  define IE_SOFT_BIT   (1ULL << 3)   /* mie.MSIE */

#  define CAUSE_EXTERNAL  11
#  define CAUSE_TIMER     7
#  define CAUSE_SOFTWARE  3

#endif

/* Common across modes. */
#define STATUS_FS_INITIAL   (1ULL << 13)   /* xstatus.FS = 01 (Initial) */
#define CAUSE_INTERRUPT_BIT (1ULL << 63)   /* RV64: bit XLEN-1 */

/* Inline-asm CSR-name stringification.
 *
 * The C preprocessor doesn't expand identifiers inside asm string
 * literals, so to write generic inline asm we emit two strings and let
 * the compiler concatenate them:
 *
 *     __asm__ volatile ("csrr %0, " HAL_CSR(CSR_STATUS)
 *                       : "=r"(v));
 *
 * HAL_CSR(CSR_STATUS) → HAL_STR_(mstatus) → "mstatus".
 * Inside .S files, just `csrr t0, CSR_STATUS` — no stringify needed,
 * the assembler accepts the bare token after cpp expansion. */
#define HAL_STR_(x) #x
#define HAL_CSR(x)  HAL_STR_(x)
