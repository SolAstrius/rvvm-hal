/* Real-time clock — Google Goldfish RTC, the device RVVM exposes for
 * wallclock time. Pairs with `time.h`'s monotonic mtime CSR — that
 * one ticks at boot from zero, this one returns Unix epoch seconds.
 *
 * Register layout (RVVM src/devices/rtc-goldfish.c):
 *   0x00  TIME_LOW    — read latches the high half of the 64-bit ns timestamp
 *   0x04  TIME_HIGH   — read the high half latched at the prior LOW read
 *   0x08  ALARM_LOW   — alarm time low bits (we don't use)
 *   0x0C  ALARM_HIGH  — alarm time high bits
 *   0x10  IRQ_ENABLED
 *   0x14  ALARM_CLEAR
 *   0x18  ALARM_STATUS
 *
 * Discovery: FDT compatible = "google,goldfish-rtc". Pass 0 to
 * rtc_init() and it falls back to the default 0x101000.
 *
 * Glue: this is what FatFs's get_fattime needs to flip from the
 * boot-date stub to real wallclock, and what picolibc's time()
 * /gmtime() / strftime() should be reading. */

#pragma once
#include <stdint.h>

/* Initialise. Pass MMIO base address discovered from FDT, or 0 to
 * fall back to RVVM's default 0x101000. Idempotent — safe to call
 * multiple times. */
void rtc_init(uintptr_t base);

/* Wallclock as nanoseconds since Unix epoch. RVVM reflects the
 * host's clock_gettime(CLOCK_REALTIME) at read time, so the value
 * advances at host-rate (not RVVM mtime rate). */
uint64_t rtc_now_ns(void);

/* Convenience: seconds since Unix epoch. */
static inline uint64_t rtc_now_seconds(void) {
    return rtc_now_ns() / 1000000000ULL;
}
