/* Read RISC-V mtime via the unprivileged `time` CSR. RVVM tick rate
 * and frame deadline live in rvvm.h. */

#pragma once
#include <stdint.h>
#include "rvvm.h"

/* Compatibility aliases — keep older code working. */
#define TIME_HZ            RVVM_TIME_HZ
#define TICKS_PER_FRAME    RVVM_TICKS_PER_FRAME

static inline uint64_t time_now(void) {
    uint64_t v;
    __asm__ volatile ("rdtime %0" : "=r"(v));
    return v;
}

/* Idle-wait until `deadline`. Uses RISC-V's `wfi` to park the CPU
 * instead of burning cycles in a NOP loop — RVVM yields the host
 * thread on wfi, so a 60 fps firmware loop consumes near-zero host
 * CPU between frames.
 *
 * Mechanism: program ACLINT mtimecmp to `deadline`; enable mie.MTIE
 * so RVVM's wfi handler computes the wake delay from mtimecmp;
 * issue wfi. When mtime crosses mtimecmp, MTIP gets set in mip —
 * that's a "pending" condition wfi wakes on. Crucially we DON'T
 * touch mstatus.MIE: with mstatus.MIE off the pending interrupt
 * never traps, so we don't need a timer IRQ handler. The pending
 * bit alone unparks the CPU.
 *
 * (RVVM-specific subtlety: src/riscv_priv.c only consults mtimecmp
 * when mie.MTIE is set; without it wfi sleeps for CONDVAR_INFINITE
 * and the firmware hangs forever. Hence the csrs/csrc dance.)
 *
 * The outer while-loop guards against spurious wake-ups (the spec
 * permits wfi to return for implementation-defined reasons too).
 * On exit we push mtimecmp out to "never" and clear MTIE so a
 * later irq.c that turns on mstatus.MIE can't trip a stale MTIP. */
static inline void time_busy_until(uint64_t deadline) {
    volatile uint64_t *mtimecmp = (volatile uint64_t *)RVVM_MTIMECMP(0);
    uintptr_t mtie = 1ULL << 7;   /* mie.MTIE */
    __asm__ volatile ("csrs mie, %0" :: "r"(mtie));
    while (time_now() < deadline) {
        *mtimecmp = deadline;
        __asm__ volatile ("wfi");
    }
    *mtimecmp = (uint64_t)-1;
    __asm__ volatile ("csrc mie, %0" :: "r"(mtie));
}
