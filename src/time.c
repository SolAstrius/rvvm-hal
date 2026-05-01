#include "time.h"
#include "plat.h"
#include "priv.h"

/* Defaults match RVVM's stock machine. time_init() overrides at boot
 * once the firmware has parsed the FDT. */
static uint64_t time_hz_v = RVVM_TIME_HZ;

void time_init(uintptr_t clint, uint64_t hz) {
    /* CLINT base is held by the platform layer (used for both timer
     * and IPI); forward the discovery there. The frequency lives
     * here because it's a property of the time CSR, not of the
     * timer-deadline mechanism — backends without CLINT (S+SBI,
     * AIA+Sstc) still need it. */
    if (clint) plat_init(0, clint);
    if (hz)    time_hz_v = hz;
}

uint64_t time_hz(void)              { return time_hz_v; }
uint64_t time_ticks_per_frame(void) { return time_hz_v / 60; }

/* Idle-wait until `deadline` using `wfi` instead of a NOP loop. RVVM
 * yields the host thread on wfi, so a 60 fps frame loop drops idle
 * host-CPU usage to near zero.
 *
 * Mechanism: program the platform timer to `deadline`, set the timer
 * IE bit so the wfi handler computes the wake delay (RVVM gates that
 * on MTIE — without it, wfi sleeps for CONDVAR_INFINITE and the
 * firmware hangs forever). Execute wfi. When the deadline elapses,
 * the timer-interrupt-pending bit becomes 1 and wfi returns. CSR_STATUS
 * interrupt-enable stays off so no IRQ trap fires — the pending bit
 * alone unparks the CPU.
 *
 * The outer while-loop guards against spurious wake-ups (the spec
 * permits wfi to return for implementation-defined reasons). On exit
 * we push the deadline out to "never" and clear the timer IE so a
 * later irq.c that flips global IE on can't trip a stale pending. */
void time_busy_until(uint64_t deadline) {
    uintptr_t status_ie = STATUS_IE_BIT;

    /* If the firmware has globally enabled interrupts (irq.c does this
     * for UART/PLIC), our IE_TIMER_BIT will trap to the irq.c
     * dispatcher, which has no handler for the timer cause and will
     * disable IE_TIMER_BIT to break the loop — defeating our wake
     * mechanism. Clear CSR_STATUS interrupt-enable for the duration
     * of the wait so the timer pending wakes wfi but doesn't trap;
     * restore it after. */
    uintptr_t old_status;
    __asm__ volatile ("csrrc %0, " HAL_CSR(CSR_STATUS) ", %1"
                      : "=r"(old_status) : "r"(status_ie));

    plat_timer_irq_enable();
    while (time_now() < deadline) {
        plat_timer_set_deadline(deadline);
        __asm__ volatile ("wfi");
    }
    plat_timer_set_deadline((uint64_t)-1);
    plat_timer_irq_disable();

    if (old_status & STATUS_IE_BIT) {
        __asm__ volatile ("csrs " HAL_CSR(CSR_STATUS) ", %0" :: "r"(status_ie));
    }
}
