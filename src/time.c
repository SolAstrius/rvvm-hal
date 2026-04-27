#include "time.h"

/* Defaults match RVVM's stock machine. time_init() overrides at boot
 * once the firmware has parsed the FDT. */
static uintptr_t clint_base = RVVM_CLINT_BASE;
static uint64_t  time_hz_v  = RVVM_TIME_HZ;

void time_init(uintptr_t clint, uint64_t hz) {
    if (clint) clint_base = clint;
    if (hz)    time_hz_v  = hz;
}

uint64_t time_hz(void)              { return time_hz_v; }
uint64_t time_ticks_per_frame(void) { return time_hz_v / 60; }

/* Idle-wait until `deadline` using `wfi` instead of a NOP loop. RVVM
 * yields the host thread on wfi, so a 60 fps frame loop drops idle
 * host-CPU usage to near zero.
 *
 * Mechanism: program ACLINT mtimecmp to `deadline`, set mie.MTIE so
 * RVVM's wfi handler computes the wake delay from mtimecmp (riscv_priv.c
 * gates that on MTIE — without it wfi sleeps for CONDVAR_INFINITE and
 * the firmware hangs forever), execute wfi. When mtime crosses the
 * comparator, MTIP becomes pending and wfi returns. mstatus.MIE stays
 * off so no IRQ trap fires — the pending bit alone unparks the CPU.
 *
 * SiFive CLINT layout (implied by `compatible = "sifive,clint0"`,
 * not exposed in FDT): MTIMER region at base + 0x4000; mtimecmp[hartid]
 * at MTIMER + hartid*8. We're hart 0, so + 0x4000 + 0.
 *
 * The outer while-loop guards against spurious wake-ups (the spec
 * permits wfi to return for implementation-defined reasons). On exit
 * we push mtimecmp out to "never" and clear MTIE so a later irq.c that
 * flips mstatus.MIE on can't trip a stale MTIP. */
void time_busy_until(uint64_t deadline) {
    volatile uint64_t *mtimecmp =
        (volatile uint64_t *)(clint_base + 0x4000);
    uintptr_t mtie = 1ULL << 7;   /* mie.MTIE */
    uintptr_t mie  = 1ULL << 3;   /* mstatus.MIE */

    /* If the firmware has globally enabled M-mode interrupts (irq.c
     * calls this for UART/PLIC), our MTIE will trap to the irq.c
     * dispatcher, which has no handler for MTIP and will disable MTIE
     * to break the loop — defeating our wake mechanism. Clear
     * mstatus.MIE for the duration of the wait so MTIP wakes wfi but
     * doesn't trap; restore it after. */
    uintptr_t old_status;
    __asm__ volatile ("csrrc %0, mstatus, %1" : "=r"(old_status) : "r"(mie));

    __asm__ volatile ("csrs mie, %0" :: "r"(mtie));
    while (time_now() < deadline) {
        *mtimecmp = deadline;
        __asm__ volatile ("wfi");
    }
    *mtimecmp = (uint64_t)-1;
    __asm__ volatile ("csrc mie, %0" :: "r"(mtie));

    if (old_status & mie) {
        __asm__ volatile ("csrs mstatus, %0" :: "r"(mie));
    }
}
