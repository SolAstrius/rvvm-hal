/* Multi-hart support — opt-in. By default secondary harts park in a
 * wfi loop with mie.MSIE set; the firmware can wake them by calling
 * smp_start(hartid, fn, arg), which writes the function pointer + arg
 * into a per-hart command slot and pulses the CLINT MSIP register for
 * that hart. Secondary wakes from wfi, dispatches into fn, returns,
 * goes back to wfi.
 *
 * RISC-V SMP shape (different from x86 INIT/SIPI): every hart starts
 * simultaneously at the reset PC and decides what to do based on
 * `mhartid`. There's no AP boot handshake — secondaries simply park
 * themselves at the bottom of start.S and wait for the primary to
 * tell them what's expected.
 *
 * Wakeup mechanism: SiFive CLINT exposes a 4-byte `msip[hartid]`
 * register at base + hartid*4 (see rvvm.h CLINT layout). With
 * mie.MSIE set on the secondary, a non-zero write to msip raises
 * MSIP-pending and unparks wfi. The secondary's start.S park loop
 * polls msip after wake and, if non-zero, clears it and dispatches.
 *
 * Under the S+SBI backend (HAL_PLAT=s_sbi) the shape is the same but
 * the primitives differ: secondaries are brought up via sbi_hart_start
 * (HSM) into start_s.S's _smp_secondary_entry, wakes arrive as
 * sip.SSIP via sbi_send_ipi, and hartid comes from SBI (in tp) rather
 * than mhartid. The smp.c logic above the plat layer is unchanged.
 *
 * Constraints:
 *  - At most SMP_MAX_HARTS harts. Each gets a 16 KiB stack reserved
 *    by link.ld; harts beyond that bound park forever and never run
 *    user code.
 *  - Secondaries run with mstatus.MIE off and (after smp_start) the
 *    same __trap_entry as the primary, so synchronous exceptions get
 *    a register-dump panic via the same mechanism. External IRQs
 *    don't deliver to secondaries (their PLIC ctx isn't enabled).
 *  - smp_start is one-shot: only call it again on a hart after the
 *    previous entry has returned (poll smp_is_running() or just
 *    smp_wait()). Concurrent UART output between harts is fine for
 *    short messages but unsynchronised; consumers that hammer the
 *    UART from multiple harts should add their own lock.
 *
 * Default workflow for a parallelisable consumer (e.g. an emulator
 * core that wants to run a CPU step on a worker hart):
 *
 *     fdt_t fdt; fdt_init(&fdt, fdt_blob);
 *     smp_init(&fdt);
 *     uart_printf("smp: %u harts\n", smp_hart_count());
 *
 *     // dispatch one frame's worth of work onto hart 1:
 *     smp_start(1, run_emulator_step, &state);
 *     do_other_work_on_primary();
 *     smp_wait(1);
 *
 * Consumers that don't care about SMP do nothing: secondaries stay
 * parked, primary runs as before. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"

/* Maximum hart count we can stack-allocate for. Each hart gets a
 * 16 KiB stack reserved at link time (link.ld), so this caps total
 * stack reservation at SMP_MAX_HARTS × 16 KiB = 128 KiB.
 *
 * RVVM defaults `-smp 1`. Common multicore choices are 2, 4, 8.
 * Setting -smp larger than this just parks the extra harts forever
 * — they never get a stack so we can't run code on them. */
#define SMP_MAX_HARTS  8

/* Per-hart stack size. Must match the value used in link.ld and
 * start.S (the asm-side number is hardcoded; this header is the
 * source of truth that callers might reference for sizing decisions). */
#define SMP_STACK_PER_HART  (16 * 1024)

typedef void (*smp_entry_t)(uint64_t hartid, void *arg);

#ifdef HAL_NO_SMP
/* Single-hart build: smp.c isn't compiled and start.S parks every
 * non-zero hart immediately. The API stays present as inline stubs so
 * consumers can write the same code that works in either build mode —
 * smp_hart_count() returns 1, smp_start() returns false, smp_wait()
 * is a no-op. The compiler eliminates the unreachable branches. */
static inline void     smp_init(const fdt_t *fdt) { (void)fdt; }
static inline uint32_t smp_hart_count(void)       { return 1; }
static inline uint32_t smp_this_hart(void) {
    uint64_t v;
#ifdef HAL_PRIV_S
    /* mhartid is M-only; under SBI the hartid lives in tp (stashed by
     * start_s.S). Reading mhartid here would trap. */
    __asm__ volatile ("mv %0, tp" : "=r"(v));
#else
    __asm__ volatile ("csrr %0, mhartid" : "=r"(v));
#endif
    return (uint32_t)v;
}
static inline bool     smp_start(uint32_t hartid, smp_entry_t fn, void *arg) {
    (void)hartid; (void)fn; (void)arg; return false;
}
static inline bool     smp_is_running(uint32_t hartid) { (void)hartid; return false; }
static inline void     smp_wait(uint32_t hartid)       { (void)hartid; }
#else

/* Discover the hart count from the device tree (counts /cpus children
 * with device_type = "cpu") and prepare the dispatch tables. Must be
 * called from the primary hart after fdt_init. Without this, hart
 * count defaults to 1 and smp_start fails for any non-zero hart. */
void smp_init(const fdt_t *fdt);

/* Total harts visible in the FDT, capped at SMP_MAX_HARTS. Always >= 1
 * (the primary itself counts). */
uint32_t smp_hart_count(void);

/* Read mhartid. Useful inside smp_entry_t functions. */
uint32_t smp_this_hart(void);

/* Hand a function + arg to the named secondary hart and wake it. The
 * function runs on that hart with a fresh stack; when it returns the
 * hart parks again and is available for another smp_start.
 *
 * Returns false if `hartid` is 0 (the primary), >= smp_hart_count(),
 * or already running an entry. The fn/arg pair is consumed atomically:
 * either it's accepted and queued, or smp_start returns false and
 * touches nothing. */
bool smp_start(uint32_t hartid, smp_entry_t fn, void *arg);

/* Is `hartid` currently executing a queued entry? Lockless read; safe
 * to call from any hart. */
bool smp_is_running(uint32_t hartid);

/* Spin (busy, not wfi — typically short waits) until `hartid` finishes
 * its current entry. No-op if the hart was idle to begin with. */
void smp_wait(uint32_t hartid);

#endif /* HAL_NO_SMP */
