/* Panic, exit, and assertion primitives.
 *
 * Two callable entry points:
 *
 *   hal_panic(fmt, ...)   — formatted firmware-initiated panic. Prints
 *                           the message, captures the current call site
 *                           (return address + frame pointer), then dumps
 *                           the same structured frame as a synchronous
 *                           hardware exception. wfi forever.
 *
 *   hal_exit(code)        — clean shutdown. Logs the code, then writes
 *                           SYSCON_POWEROFF to RVVM's syscon device,
 *                           which ends RVVM's eventloop. The host RVVM
 *                           process exits 0 — `code` itself is logged
 *                           but not propagated, since RVVM's syscon is
 *                           a one-bit channel (off/reset only). CI greps
 *                           the UART transcript for the marker line.
 *
 * Plus two macros built on hal_panic:
 *
 *   HAL_CHECK(cond)       — always-live assertion. Failures panic with
 *                           file:line and the stringified condition.
 *                           Use for runtime invariants that must hold
 *                           regardless of build profile (e.g. "queue
 *                           depth fits in a u8 before truncation").
 *
 *   HAL_ASSERT(cond)      — debug-only. Compiles to no-op without
 *                           HAL_DEBUG. Use for development-time sanity
 *                           checks too costly to run in release.
 *
 * Output format is documented and stable: every panic produces a
 * !!HAL-PANIC-V1 frame with one register per line, decoded mcause,
 * and a fp-walked backtrace. tools/decode-panic resolves the
 * addresses against an ELF using addr2line. See README's Debugging
 * section.
 *
 * Re-entry: if a panic recurses (e.g., a fault inside the dumper
 * itself), the second call short-circuits to wfi without re-printing.
 * Multi-hart: SMP builds funnel every hart's panics through the same
 * UART chokepoint with a per-hart prefix; a panicking hart wfi-loops
 * but does not signal others — surviving harts keep running, which
 * is the right call for CI debugging where you want to read the dump
 * unobstructed. */

#pragma once

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format and panic. Never returns. fmt must be non-NULL; pass "" for
 * a panic with no narrative. */
__attribute__((noreturn, format(printf, 1, 2)))
void hal_panic(const char *fmt, ...);

/* Same, accepting a va_list — for wrappers that want their own
 * variadic prefix (HAL_CHECK, picolibc abort path, future tracers). */
__attribute__((noreturn))
void hal_panic_v(const char *fmt, va_list ap);

/* Clean shutdown via syscon. Code is logged but not propagated to
 * the host process (see header comment). Convention: 0 means success,
 * non-zero means failure; CI greps for "EXIT 0" / "EXIT N" markers. */
__attribute__((noreturn))
void hal_exit(int code);

/* Hardware-trap entry into the panic dumper. Called from
 * trap_dispatch (src/irq.c) when a synchronous exception (load fault,
 * illegal instruction, …) reaches us with no firmware-installed
 * handler. `regs` points at the 32×8 GPR save area built by
 * __trap_entry (start.S); see that file for slot layout. Firmware
 * code should not call this directly — call hal_panic instead. */
__attribute__((noreturn))
void hal_panic_from_trap(uint64_t mcause, uint64_t mepc, uint64_t *regs);

/* Always-live runtime check. Compiles to a branch + tail call to
 * hal_panic on failure. Cheap on the hot path (single mispredicted
 * branch only when the invariant is violated). */
#define HAL_CHECK(cond) do {                                          \
    if (__builtin_expect(!(cond), 0)) {                               \
        hal_panic("CHECK failed: %s\n   at %s:%d (in %s)",            \
                  #cond, __FILE__, __LINE__, __func__);               \
    }                                                                 \
} while (0)

/* Debug-only assertion. Compiled out without -DHAL_DEBUG. */
#ifdef HAL_DEBUG
#define HAL_ASSERT(cond) HAL_CHECK(cond)
#else
#define HAL_ASSERT(cond) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
