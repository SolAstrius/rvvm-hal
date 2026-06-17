/* plat.h — platform abstraction for privilege/IRQ-controller-specific
 * machinery.
 *
 * Three pieces of the HAL depend on what the underlying platform
 * exposes for privileged operations:
 *
 *   1. IPI delivery (wake another hart from wfi-park).
 *   2. Timer (one-shot deadline + per-hart enable).
 *   3. External interrupt routing (claim/complete + per-source masks).
 *
 * The privilege-portable code (irq.c, smp.c, time.c) calls into this
 * header; one .c file per backend implements it. Exactly one backend
 * is linked into libhal.a, picked by the Makefile's HAL_PLAT variable.
 *
 * Backends (implemented):
 *   plat_m_clint.c   M-mode + CLINT (IPI/timer) + PLIC (intc).
 *                    Default; the stock RVVM target.
 *   plat_s_sbi.c     S-mode payload under OpenSBI: ecall-based IPI,
 *                    sbi_set_timer (with Sstc fast-path), PLIC at the
 *                    S-mode context. Hart bring-up via sbi_hart_start
 *                    (HSM extension). Built by examples/probe-s.
 *
 * Planned (not yet implemented):
 *   plat_m_clic.c    M-mode + CLIC (vectored, per-IRQ priority regs).
 *                    No claim/complete: claim() reads the hardware-
 *                    presented IRQ ID, complete() is a no-op.
 *   plat_s_aia.c     S-mode + IMSIC (per-hart MSI file) + APLIC +
 *                    Sstc. claim() drains the IMSIC EID queue.
 *
 * Adding a backend is "one .c file, no header changes": every
 * platform-specific symbol the rest of the HAL needs is declared here.
 *
 * Discovery: plat_init runs once on the primary after FDT parse,
 * passing whatever base addresses the FDT yielded (or 0 to fall back
 * to the backend's compiled-in defaults). Backends that don't need a
 * base (S+SBI uses ecalls, not MMIO) ignore the arguments. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"

/* Lifecycle. Call once on the primary, after FDT parse, before
 * irq_init / smp_init / time_init. Pass 0 for either argument to keep
 * the backend's default (RVVM_PLIC_BASE / RVVM_CLINT_BASE for the
 * stock RVVM machine). */
void plat_init(uintptr_t intc_base, uintptr_t clint_base);

/* ====================================================================
 *  IPI — inter-processor interrupt
 * ==================================================================== */

/* Wake `hartid` from its wfi-park. The receiving hart's wake path is
 * backend-defined: M+CLINT pulses MSIP and the secondary's start.S
 * loop polls msip after wake; S+SBI sends an ecall and the secondary's
 * sip.SSIP becomes pending. The boot park loop (start.S / start_s.S)
 * clears its own pending bit inline in asm on each wake; the runtime
 * trap dispatcher uses plat_ipi_ack for the same purpose. */
void plat_ipi_send(uint32_t hartid);

/* Acknowledge (clear) a pending IPI on the calling hart. Called from
 * the secondary after it observes the wake and reads its smp_cmd
 * slot, before going back to wfi. */
void plat_ipi_ack(uint32_t self_hartid);

/* ====================================================================
 *  Timer — one-shot deadline + per-hart enable
 * ==================================================================== */

/* Arm a deadline (in `time_now()` ticks). When `time_now() >= deadline`
 * the timer interrupt pends in CSR_IP. Pass UINT64_MAX to disarm. */
void plat_timer_set_deadline(uint64_t deadline);

/* Toggle the per-hart timer-interrupt enable bit (mie.MTIE / sie.STIE).
 * The bit position differs across privilege modes — handled by
 * priv.h's IE_TIMER_BIT macro. */
void plat_timer_irq_enable(void);
void plat_timer_irq_disable(void);

/* ====================================================================
 *  Interrupt controller — external IRQ routing
 *
 *  irq.c maintains a privilege-agnostic dispatch table keyed by source
 *  number. The backend handles claim/complete and per-source masks.
 *  Source numbers are platform-defined (PLIC IRQ numbers today,
 *  IMSIC EIDs under AIA, CLIC IRQ-IDs under CLIC).
 * ==================================================================== */

/* Reset the controller: disable every source for our context, zero
 * priorities, set threshold = 0. Called from irq_init. */
void plat_intc_init(void);

void plat_intc_enable_source(uint32_t source);
void plat_intc_disable_source(uint32_t source);
void plat_intc_set_priority(uint32_t source, uint32_t prio);
void plat_intc_set_threshold(uint32_t threshold);

/* Returns 0 when nothing is pending. The dispatcher loops on this
 * until it drains. */
uint32_t plat_intc_claim(void);
void     plat_intc_complete(uint32_t source);

/* Source-number bound (largest valid source + 1). Backends report the
 * controller's hard limit so irq.c can size its dispatch table. */
uint32_t plat_intc_source_limit(void);

/* ====================================================================
 *  Hart identity
 *
 *  csrr mhartid is M-only. S-mode receives hartid in a0 from SBI and
 *  conventionally stashes it in tp. Backends abstract the read so
 *  callers don't care which mechanism is in use.
 * ==================================================================== */
uint32_t plat_this_hart(void);

/* ====================================================================
 *  Secondary hart bring-up
 *
 *  In the M+CLINT model, all harts simultaneously enter at the reset
 *  PC; start.S parks the non-primaries into a wfi-loop waiting for
 *  MSIP. By the time the primary calls smp_init(), every secondary
 *  is already parked — so M+CLINT implements this as a no-op.
 *
 *  Under SBI HSM, only the boot hart starts; non-primaries are in
 *  STOPPED state. The primary must call sbi_hart_start to wake each
 *  secondary into the equivalent park loop (start_s.S's
 *  _smp_secondary_entry). plat_s_sbi.c implements this with one ecall
 *  per hart.
 *
 *  smp_init calls this once per non-primary hart after FDT discovery,
 *  before any user code calls smp_start. After return, every
 *  secondary is parked and reachable via plat_ipi_send. */
void plat_hart_bringup_park(uint32_t hartid);

/* ====================================================================
 *  Runtime feature probe
 *
 *  Some backends benefit from inspecting the FDT for hardware features
 *  that change the fast path (Sstc → direct `csrw stimecmp` instead of
 *  `sbi_set_timer` ecall; AIA → IMSIC instead of PLIC; …). This hook
 *  lets the consumer hand the FDT to the backend after fdt_init,
 *  before time_init / irq_init. No-op for backends that don't care
 *  (M+CLINT today).
 *
 *  Safe to call zero or multiple times. Each call re-probes; later
 *  calls override earlier ones. Cheap (one FDT walk over the cpus
 *  node), so consumers may invoke it unconditionally. */
void plat_probe_features(const fdt_t *fdt);
