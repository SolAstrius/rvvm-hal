/* Atomic operations on RISC-V using the A extension.
 *
 * RVVM advertises rv64imafdc by default — `a` is in the base set.
 * These helpers wrap the AMO instructions and the LR/SC pair into
 * typed C inlines so consumers don't write inline asm.
 *
 * Memory ordering: every operation is sequentially consistent
 * (`aqrl` modifier). The HAL doesn't expose relaxed/acquire/release
 * variants because they mostly muddy the contract — if you need
 * tighter ordering for performance, drop into inline asm yourself.
 *
 * Types: 32-bit and 64-bit variants. RV64 has `amoadd.d`, `lr.d`,
 * `sc.d` for 64-bit; `amoadd.w` etc. for 32-bit. Most callers want
 * 32-bit (counters, flags) — atomic_*_u32 maps there. Pointers and
 * 64-bit accumulators use atomic_*_u64.
 *
 * Conventions:
 *   atomic_load_u32(&v)        sequentially-consistent load
 *   atomic_store_u32(&v, x)    sequentially-consistent store
 *   atomic_add_u32(&v, x)      v += x; returns OLD value
 *   atomic_sub_u32(&v, x)      v -= x; returns OLD value (via add of -x)
 *   atomic_and_u32(&v, x)      v &= x; returns OLD value
 *   atomic_or_u32(&v, x)       v |= x; returns OLD value
 *   atomic_xor_u32(&v, x)      v ^= x; returns OLD value
 *   atomic_xchg_u32(&v, x)     v = x; returns OLD value
 *   atomic_cas_u32(&v,exp,new) compare-and-swap; returns true if swapped
 *
 * The `volatile` cast on the location is what makes the compiler
 * trust that other harts may have changed it between accesses. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 * 32-bit
 * ==================================================================== */

static inline uint32_t atomic_load_u32(const volatile uint32_t *p) {
    /* lr.w with no following sc is a clean acquire-load on RVA. */
    uint32_t r;
    __asm__ volatile ("lr.w.aqrl %0, (%1)" : "=r"(r) : "r"(p) : "memory");
    return r;
}

static inline void atomic_store_u32(volatile uint32_t *p, uint32_t v) {
    /* amoswap throws away the old value, but gives us seq-cst. */
    uint32_t dummy;
    __asm__ volatile ("amoswap.w.aqrl %0, %2, (%1)"
                      : "=r"(dummy) : "r"(p), "r"(v) : "memory");
}

static inline uint32_t atomic_add_u32(volatile uint32_t *p, uint32_t x) {
    uint32_t old;
    __asm__ volatile ("amoadd.w.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(x) : "memory");
    return old;
}

static inline uint32_t atomic_sub_u32(volatile uint32_t *p, uint32_t x) {
    return atomic_add_u32(p, (uint32_t)(-(int32_t)x));
}

static inline uint32_t atomic_and_u32(volatile uint32_t *p, uint32_t x) {
    uint32_t old;
    __asm__ volatile ("amoand.w.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(x) : "memory");
    return old;
}

static inline uint32_t atomic_or_u32(volatile uint32_t *p, uint32_t x) {
    uint32_t old;
    __asm__ volatile ("amoor.w.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(x) : "memory");
    return old;
}

static inline uint32_t atomic_xor_u32(volatile uint32_t *p, uint32_t x) {
    uint32_t old;
    __asm__ volatile ("amoxor.w.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(x) : "memory");
    return old;
}

static inline uint32_t atomic_xchg_u32(volatile uint32_t *p, uint32_t v) {
    uint32_t old;
    __asm__ volatile ("amoswap.w.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}

/* Compare-and-swap. Returns true if *p was equal to expected and
 * has been replaced by desired. On failure, *p is unchanged. The
 * RV LR/SC sequence is bounded — sc.w can fail spuriously, hence
 * the loop. We bound retries at 32 to avoid a livelock if some
 * other hart is also CAS-ing the same address; callers that need
 * progress under contention should retry at the application level. */
static inline bool atomic_cas_u32(volatile uint32_t *p,
                                  uint32_t expected, uint32_t desired) {
    uint32_t cur;
    int retries = 32;
    do {
        __asm__ volatile ("lr.w.aqrl %0, (%1)" : "=r"(cur) : "r"(p) : "memory");
        if (cur != expected) return false;
        uint32_t fail;
        __asm__ volatile ("sc.w.aqrl %0, %2, (%1)"
                          : "=r"(fail) : "r"(p), "r"(desired) : "memory");
        if (fail == 0) return true;
    } while (--retries > 0);
    return false;
}

/* ====================================================================
 * 64-bit
 * ==================================================================== */

static inline uint64_t atomic_load_u64(const volatile uint64_t *p) {
    uint64_t r;
    __asm__ volatile ("lr.d.aqrl %0, (%1)" : "=r"(r) : "r"(p) : "memory");
    return r;
}

static inline void atomic_store_u64(volatile uint64_t *p, uint64_t v) {
    uint64_t dummy;
    __asm__ volatile ("amoswap.d.aqrl %0, %2, (%1)"
                      : "=r"(dummy) : "r"(p), "r"(v) : "memory");
}

static inline uint64_t atomic_add_u64(volatile uint64_t *p, uint64_t x) {
    uint64_t old;
    __asm__ volatile ("amoadd.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(x) : "memory");
    return old;
}

static inline uint64_t atomic_xchg_u64(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    __asm__ volatile ("amoswap.d.aqrl %0, %2, (%1)"
                      : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}

static inline bool atomic_cas_u64(volatile uint64_t *p,
                                  uint64_t expected, uint64_t desired) {
    uint64_t cur;
    int retries = 32;
    do {
        __asm__ volatile ("lr.d.aqrl %0, (%1)" : "=r"(cur) : "r"(p) : "memory");
        if (cur != expected) return false;
        uint64_t fail;
        __asm__ volatile ("sc.d.aqrl %0, %2, (%1)"
                          : "=r"(fail) : "r"(p), "r"(desired) : "memory");
        if (fail == 0) return true;
    } while (--retries > 0);
    return false;
}

/* Cross-hart memory + I/O fence. Useful for hand-rolled producer/
 * consumer queues where atomic ops cover one side but the other
 * side needs ordering with respect to plain memory. */
static inline void atomic_fence(void) {
    __asm__ volatile ("fence rw, rw" ::: "memory");
}
