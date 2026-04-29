#include "rtc.h"
#include "mmio.h"

/* RVVM's default base for goldfish-rtc — match if FDT discovery
 * isn't run. From RVVM src/devices/rtc-goldfish.h's
 * RTC_GOLDFISH_ADDR_DEFAULT. */
#define RTC_DEFAULT_BASE  0x101000UL

#define RTC_TIME_LOW   0x00
#define RTC_TIME_HIGH  0x04

static uintptr_t rtc_base = RTC_DEFAULT_BASE;

void rtc_init(uintptr_t base) {
    if (base) rtc_base = base;
}

uint64_t rtc_now_ns(void) {
    /* RVVM updates time_low + time_high atomically inside the
     * TIME_LOW read handler. Read low first to latch high; then
     * read high. Without that order the high half could be from
     * an older sample. */
    uint32_t lo = mmio_r32(rtc_base + RTC_TIME_LOW);
    uint32_t hi = mmio_r32(rtc_base + RTC_TIME_HIGH);
    return ((uint64_t)hi << 32) | lo;
}
