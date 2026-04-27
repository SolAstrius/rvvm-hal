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
 * thread on wfi, so a 60 fps firmware loop now consumes near-zero
 * host CPU between frames.
 *
 * Mechanism: program ACLINT mtimecmp to `deadline`; wfi waits until
 * any interrupt becomes pending. When mtime reaches mtimecmp, MTIP
 * is set in mip — that's a "pending" condition wfi wakes on, even
 * if mie.MTIE / mstatus.MIE are off. So we don't need to wire a
 * timer-IRQ handler; the pending bit alone unparks the CPU.
 *
 * The outer while-loop guards against spurious wake-ups (the spec
 * permits wfi to return for implementation-defined reasons too).
 *
 * After we exit, push mtimecmp out to "never" so a leftover MTIP
 * doesn't stay pending and confuse later code (e.g. the irq.c
 * dispatcher) that might enable timer interrupts. */
static inline void time_busy_until(uint64_t deadline) {
    volatile uint64_t *mtimecmp = (volatile uint64_t *)RVVM_MTIMECMP(0);
    while (time_now() < deadline) {
        *mtimecmp = deadline;
        __asm__ volatile ("wfi");
    }
    *mtimecmp = (uint64_t)-1;
}
