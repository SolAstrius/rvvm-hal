/* Monotonic time + idle-wait. RISC-V `time` CSR is the clock; CLINT
 * mtimecmp drives the wfi-paced wait. Tick rate and CLINT base default
 * to RVVM's standard machine and can be overridden via time_init() —
 * call it at boot with values discovered from FDT (compatible
 * "sifive,clint0" for the CLINT, /cpus/timebase-frequency for the rate). */

#pragma once
#include <stdint.h>
#include "rvvm.h"

static inline uint64_t time_now(void) {
    uint64_t v;
    __asm__ volatile ("rdtime %0" : "=r"(v));
    return v;
}

/* Pass 0 for either argument to keep the current value (RVVM defaults
 * on first call). Both fields are FDT-resolvable at boot. */
void time_init(uintptr_t clint_base, uint64_t hz);

uint64_t time_hz(void);
uint64_t time_ticks_per_frame(void);   /* hz / 60 */

/* Park the CPU on `wfi` until `deadline` ticks. Near-zero host CPU use
 * between frames — see implementation for the mtimecmp/MTIE dance. */
void time_busy_until(uint64_t deadline);
