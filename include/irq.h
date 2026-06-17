/* Interrupt subsystem — RISC-V trap entry + SiFive PLIC driver +
 * dispatch table.
 *
 * Wires `CSR_TVEC` to a register-saving trampoline in `trap.S` that
 * forwards into a C dispatcher. External interrupts (cause = 11 in
 * M-mode, 9 in S-mode) are
 * funnelled through the PLIC's claim/complete protocol; synchronous
 * exceptions (load fault, illegal instruction, ecall, …) panic with a
 * register dump.
 *
 * Typical wiring (after FDT discovery, mirroring the chip-8 init
 * sequence):
 *
 *     irq_init(plic_base);                  // claim mtvec, configure PLIC
 *     irq_register(uart_line, on_uart, NULL);
 *     irq_set_priority(uart_line, 3);
 *     irq_enable(uart_line);
 *     irq_global_enable();                  // mstatus.MIE | mie.MEIE
 *
 * `uart_line` comes from the UART's FDT node `interrupts` property, or
 * from PCI config offset 0x3C (PCI_CFG_INTLINE) for PCI devices —
 * RVVM's host bridge auto-fills it with the PLIC source number when
 * the function is attached. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Handler shape. Runs with global interrupts disabled (CPU clears
 * mstatus.MIE on trap entry). The dispatcher claims+completes against
 * the PLIC around each call, so handlers don't touch claim/complete.
 * The handler body is responsible for clearing the device-side pending
 * condition (e.g. reading the UART's RBR for the RX-data IRQ) before
 * returning — otherwise the PLIC re-asserts immediately. */
typedef void (*irq_handler_t)(uint32_t source, void *ctx);

/* Initialise. `plic_base` should come from FDT (compatible
 * "sifive,plic-1.0.0", first reg cell); pass 0 to fall back to
 * RVVM_PLIC_BASE. Sets mtvec to the trap entry, zeros the dispatch
 * table, sets the M-mode context's threshold to 0 (accept all
 * non-disabled priorities), but leaves global interrupts disabled —
 * call irq_global_enable() once handlers are wired. */
void irq_init(uintptr_t plic_base);

/* Register a handler for `source` (1..RVVM_PLIC_SRC_LIMIT-1). Replaces
 * any prior handler. Pass NULL to remove. */
void irq_register(uint32_t source, irq_handler_t handler, void *ctx);

/* Enable / disable delivery of `source` to the M-mode context.
 * Without enable, the IRQ stays pending in the PLIC but never traps.
 * Default is disabled. */
void irq_enable(uint32_t source);
void irq_disable(uint32_t source);

/* Set the priority for `source` (1..7; 0 disables in PLIC terms).
 * Default after irq_init is 1 (lowest non-zero), which beats the
 * threshold of 0 — so any registered+enabled IRQ delivers. Higher
 * priority always wins on simultaneous claim. */
void irq_set_priority(uint32_t source, uint32_t prio);

/* Set the M-mode context's threshold. IRQs with prio <= threshold are
 * suppressed. Default is 0. */
void irq_set_threshold(uint32_t threshold);

/* Toggle global interrupt delivery. irq_global_enable sets
 * mstatus.MIE and mie.MEIE; irq_global_disable clears mstatus.MIE.
 * The pair returns the previous mstatus.MIE state, useful for
 * irq_save / irq_restore-style critical sections. */
bool irq_global_enable(void);
bool irq_global_disable(void);

/* Counters maintained by the dispatcher — useful as a "did anything
 * fire at all" smoke test. Lockless reads; counters are 32-bit and
 * may wrap. */
uint32_t irq_count_total(void);
uint32_t irq_count_for(uint32_t source);

/* ====================================================================
 *  Local-interrupt handlers — timer and software (IPI)
 *
 *  External (device) interrupts dispatch by PLIC source through
 *  irq_register(). The two other standard RISC-V interrupt causes —
 *  timer and software — are each delivered to a single registered
 *  handler. Registering NULL restores the dispatcher's built-in
 *  default for that cause:
 *
 *    - timer:    disable the timer IE bit, so a deadline that fires
 *                with no handler can't storm the dispatcher. (This is
 *                why time.c's wfi-pacing masks global IE rather than
 *                relying on a handler.)
 *    - software: acknowledge (clear) the IPI on this hart.
 *
 *  Both run in interrupt context with global interrupts disabled, so
 *  keep them short. The dispatcher always clears the pending condition
 *  first (timer: nothing to clear at the controller — the handler must
 *  re-arm or disarm the deadline; software: plat_ipi_ack runs before
 *  the handler), then calls the registered handler if one is set. A
 *  timer handler that wants periodic ticks must re-arm the deadline
 *  itself via plat_timer_set_deadline(). */
typedef void (*irq_local_handler_t)(void *ctx);
void irq_register_timer(irq_local_handler_t handler, void *ctx);
void irq_register_ipi(irq_local_handler_t handler, void *ctx);

/* Enable / disable delivery of the software (IPI) interrupt to this
 * hart's trap handler — sets / clears mie.MSIE (sie.SSIE in S-mode).
 * irq_init() does NOT enable it (it arms only the external IE bit), so
 * an IPI raised via plat_ipi_send() will wake wfi but won't trap into an
 * irq_register_ipi() handler until this is called. Pair with
 * irq_global_enable() for the mstatus.MIE side. Timer delivery has the
 * equivalent pair in plat.h (plat_timer_irq_enable/disable). */
void irq_ipi_enable(void);
void irq_ipi_disable(void);
