/* Panic / exit / shared dumper.
 *
 * Two entry paths converge on dump_and_halt:
 *
 *   hardware trap   →  irq.c:trap_dispatch  →  hal_panic_from_trap
 *   firmware code   →  hal_panic / HAL_CHECK / HAL_ASSERT
 *
 * Both fill a panic_frame_t with mcause / mepc / mtval / mstatus /
 * mhartid + 32 GPRs, then call dump_and_halt which:
 *   - prints a stable !!HAL-PANIC-V1 marker (versioned so future
 *     format changes don't silently break tools/decode-panic)
 *   - decodes mcause to a human name
 *   - prints all 32 GPRs in two columns
 *   - walks the frame-pointer chain bounded by __stack_bottom /
 *     __stack_top, printing up to 16 return addresses
 *   - emits a terminating end-of-panic line
 *   - wfi forever
 *
 * Re-entry: the first hart to enter sets panic_in_progress. Anyone
 * arriving second (a fault inside the dumper itself, or a parallel
 * panic on another hart) skips the printing and just halts — keeps
 * the on-wire dump readable and avoids infinite recursion if UART
 * itself is what's broken.
 *
 * Multi-hart: the panicking hart stops, but doesn't reach into other
 * harts. Surviving harts keep running. CI that boots multi-hart
 * firmwares should treat any HAL-PANIC line as a hard failure
 * regardless of which hart printed it. */

#include "panic.h"
#include "priv.h"
#include "plat.h"
#include "uart.h"
#include "rvvm.h"
#include "mmio.h"
#include "atomic.h"
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

extern char __stack_bottom[];
extern char __stack_top[];

/* Re-entry guard. Lockless test-and-set; the loser just halts
 * silently. Not a real mutex — we don't ever release this. */
static volatile uint32_t panic_in_progress = 0;

/* mcause synchronous exception names. Anything ≥ 16 is reported
 * numerically. `1U << 31` would be the interrupt flag — we get here
 * only after irq.c has already filtered interrupts out, so the
 * remaining values fit in this table. Source: RISC-V Privileged
 * Spec, "Machine Cause Register (mcause)" table. */
static const char *exc_name_for(uint64_t mcause) {
    static const char *names[16] = {
        "instruction misaligned", "instruction access fault",
        "illegal instruction",    "breakpoint",
        "load misaligned",        "load access fault",
        "store misaligned",       "store access fault",
        "ecall U",                "ecall S",
        "reserved (10)",          "ecall M",
        "instruction page fault", "load page fault",
        "reserved (14)",          "store page fault",
    };
    return mcause < 16 ? names[mcause] : "unknown";
}

/* GPR ABI mnemonics aligned with the __trap_entry frame layout in
 * start.S (slot index = register number). Index 0 is x0/zero (never
 * read or stored). Index 2 is the entry-time sp. Names are padded to
 * a fixed width so the dump column-aligns without uart_printf having
 * to support %-Ns (it doesn't). */
static const char *gpr_name(int i) {
    static const char *names[32] = {
        "x0   zero ", "x1   ra   ", "x2   sp   ", "x3   gp   ",
        "x4   tp   ", "x5   t0   ", "x6   t1   ", "x7   t2   ",
        "x8   s0fp ", "x9   s1   ", "x10  a0   ", "x11  a1   ",
        "x12  a2   ", "x13  a3   ", "x14  a4   ", "x15  a5   ",
        "x16  a6   ", "x17  a7   ", "x18  s2   ", "x19  s3   ",
        "x20  s4   ", "x21  s5   ", "x22  s6   ", "x23  s7   ",
        "x24  s8   ", "x25  s9   ", "x26  s10  ", "x27  s11  ",
        "x28  t3   ", "x29  t4   ", "x30  t5   ", "x31  t6   ",
    };
    return names[i & 31];
}

/* Print "   <NAME> : 0x<16-hex>\n". Bypasses uart_printf width specs
 * (which the tiny printf doesn't support) by using uart_put_hex64. */
static void print_reg(const char *name, uint64_t v) {
    uart_puts("   ");
    uart_puts(name);
    uart_puts(" : ");
    uart_put_hex64(v);
    uart_putc('\n');
}

/* Walk the frame-pointer chain. With -fno-omit-frame-pointer the
 * RV64 GCC/clang prologue saves ra at fp-8 and the caller's fp at
 * fp-16; fp itself points just above the current frame. We trust
 * the chain only while it stays inside [__stack_bottom, __stack_top)
 * and grows monotonically — any anomaly terminates the walk
 * silently rather than crashing the dumper.
 *
 * Builds without frame pointers (release: -Os without HAL_DEBUG)
 * print just the seed PC + ra and stop. The dumper never relies on
 * frame pointers being present. */
static void walk_stack(uintptr_t seed_pc, uintptr_t seed_ra, uintptr_t fp) {
    uart_puts("   stack:\n");
    uart_printf("     #0  0x%x\n", (uint64_t)seed_pc);

    int n = 1;
    if (seed_ra && seed_ra != seed_pc) {
        uart_printf("     #%u  0x%x\n", (uint32_t)n, (uint64_t)seed_ra);
        n++;
    }

    uintptr_t lo = (uintptr_t)__stack_bottom;
    uintptr_t hi = (uintptr_t)__stack_top;
    uintptr_t prev_fp = 0;

    for (int i = 0; i < 16 && n < 32; i++) {
        if (fp < lo + 16 || fp > hi) break;
        if (fp <= prev_fp) break;          /* chain must grow */
        if (fp & 0xF) break;               /* fp must be 16-aligned */

        uintptr_t saved_ra = *((uintptr_t *)fp - 1);
        uintptr_t saved_fp = *((uintptr_t *)fp - 2);

        if (saved_ra == 0) break;
        uart_printf("     #%u  0x%x\n", (uint32_t)n, (uint64_t)saved_ra);
        n++;

        prev_fp = fp;
        fp = saved_fp;
    }
}

/* CSR readers for context that the trap path doesn't pass through
 * the regs frame. Resolves to the right CSR for the build's privilege
 * mode via priv.h's CSR_TVAL / CSR_STATUS / CSR_TVEC aliases. The
 * dump still labels them with their M-mode names ("mtval", "mstatus",
 * "mtvec") for now — when the S-mode build lands, we'll switch the
 * labels to the active privilege's names too. */
static inline uint64_t read_xtval(void)   { uint64_t v; __asm__ volatile("csrr %0, " HAL_CSR(CSR_TVAL)   : "=r"(v)); return v; }
static inline uint64_t read_xstatus(void) { uint64_t v; __asm__ volatile("csrr %0, " HAL_CSR(CSR_STATUS) : "=r"(v)); return v; }
static inline uint64_t read_xtvec(void)   { uint64_t v; __asm__ volatile("csrr %0, " HAL_CSR(CSR_TVEC)   : "=r"(v)); return v; }

/* The shared core. `regs` is a 32-slot GPR array (slot index ==
 * register number; slot 0 unused). `ra_seed` and `fp_seed` are used
 * only when we got here from hal_panic and the trap-frame ra/fp
 * aren't applicable; pass 0 from the trap path. */
__attribute__((noreturn, noinline))
static void dump_and_halt(uint64_t  mcause,
                          uint64_t  mepc,
                          uint64_t *regs,
                          uintptr_t fp_seed) {
    /* CAS the re-entry guard. The loser halts silently. */
    if (atomic_cas_u32(&panic_in_progress, 0, 1) == false) {
        for (;;) __asm__ volatile ("wfi");
    }

    uint64_t mtval   = read_xtval();
    uint64_t mstatus = read_xstatus();
    uint64_t hartid  = plat_this_hart();
    uint64_t mtvec   = read_xtvec();

    bool is_interrupt = (mcause >> 63) & 1;
    uint64_t code = mcause & ~CAUSE_INTERRUPT_BIT;

    uart_puts("\n!!HAL-PANIC-V1 ============================================\n");
    if (is_interrupt) {
        uart_printf("   cause      : interrupt code %u (mcause=0x%x)\n",
                    (uint32_t)code, mcause);
    } else {
        uart_printf("   cause      : %s (mcause=0x%x)\n",
                    exc_name_for(code), mcause);
    }
    print_reg("mepc      ", mepc);
    print_reg("mtval     ", mtval);
    print_reg("mstatus   ", mstatus);
    print_reg("mtvec     ", mtvec);
    uart_printf("   mhartid    : %u\n", (uint32_t)hartid);
    uart_puts("   ----\n");

    /* GPRs, single column for predictable layout under the tiny
     * printf. Slot 0 is x0 (always zero). */
    for (int i = 0; i < 32; i++) {
        uint64_t v = (i == 0) ? 0 : regs[i];
        print_reg(gpr_name(i), v);
    }

    uart_puts("   ----\n");
    walk_stack((uintptr_t)mepc,
               (uintptr_t)regs[1],          /* ra */
               fp_seed ? fp_seed : (uintptr_t)regs[8]);   /* s0/fp */

    uart_puts("!!HAL-PANIC-V1 end ========================================\n");

    for (;;) __asm__ volatile ("wfi");
}

/* Hardware-trap entry: build a frame seed of 0 (use regs[8] as fp). */
void hal_panic_from_trap(uint64_t mcause, uint64_t mepc, uint64_t *regs) {
    dump_and_halt(mcause, mepc, regs, 0);
}

/* Software entry. Capture our own ra and fp, synthesise a regs frame.
 * mcause bit 63 set + low bits = 24 ("user-defined") flags this as
 * software-induced for tools/decode-panic. */
#define MCAUSE_SOFTWARE_PANIC (CAUSE_INTERRUPT_BIT | 24)

void hal_panic_v(const char *fmt, va_list ap) {
    /* Print the narrative first so it appears above the dump even if
     * the dumper itself faults. uart_vprintf is the standard hook;
     * fall back to a literal print if no fmt args. */
    uart_puts("\n!!HAL-PANIC-V1 message: ");
    if (fmt && *fmt) uart_vprintf(fmt, ap);
    uart_puts("\n");

    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);

    /* Synthesised regs: only ra (slot 1) and fp (slot 8) matter for
     * the dump path. Other slots are zeroed so the column layout
     * still prints. Stack array — small, single-shot. */
    uint64_t regs[32] = {0};
    regs[1] = ra;
    regs[8] = fp;

    /* mepc surrogate = caller's ra (the call site of hal_panic). */
    dump_and_halt(MCAUSE_SOFTWARE_PANIC, ra, regs, fp);
}

void hal_panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    hal_panic_v(fmt, ap);
    /* va_end skipped — hal_panic_v is noreturn. */
}

/* Clean shutdown. Logs the code with a stable EXIT marker, then
 * writes SYSCON_POWEROFF — RVVM ends its eventloop. If the syscon
 * device isn't mapped (custom RVVM machine without it), the write
 * traps; we wfi-loop as a fallback so CI still detects a stuck VM
 * via timeout rather than running forever. */
void hal_exit(int code) {
    uart_printf("\n!!HAL-EXIT code=%d\n", code);
    mmio_w16(RVVM_SYSCON_BASE, RVVM_SYSCON_POWEROFF);
    /* Unreachable on a properly-configured machine. */
    for (;;) __asm__ volatile ("wfi");
}
