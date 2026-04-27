/* Interrupt subsystem — RISC-V trap entry + SiFive PLIC driver +
 * dispatch table.
 *
 * Wires `mtvec` to a register-saving trampoline in `start.S` that
 * forwards into a C dispatcher. M-external interrupts (cause = 11) are
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
