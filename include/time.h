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

static inline void time_busy_until(uint64_t deadline) {
    while (time_now() < deadline) { __asm__ volatile ("nop"); }
}
