/* Mutex / spinlock primitives for SMP firmwares.
 *
 * Single-acquisition spin lock built on the A extension. This is a
 * simple ticket-free spinlock — NOT fair under contention — but it
 * fits the typical HAL consumer pattern: one hart hot in a tight
 * loop, the others occasionally taking the lock. Fairness becomes
 * important only when many harts contend for the same data, which
 * none of our consumers do today.
 *
 * Pattern:
 *
 *     #include "mutex.h"
 *
 *     static mutex_t lock = MUTEX_INIT;
 *
 *     void produce(int x) {
 *         mutex_lock(&lock);
 *         shared_state[head++] = x;
 *         mutex_unlock(&lock);
 *     }
 *
 * The lock body stores the holder's mhartid + 1 (so 0 means free)
 * which gives diagnostic value if a deadlock is found in the
 * debugger — the locked-by hart is recoverable from the value.
 *
 * No condition variables yet — those want a wait/wake primitive
 * built on CLINT MSIP, which makes more sense paired with the
 * scheduler work later. For now, callers that need wait-for-event
 * semantics can spin on an atomic flag + mutex. */

#pragma once
#include <stdbool.h>
#include "atomic.h"
#include "smp.h"

typedef struct {
    /* 0 = free; non-zero = mhartid + 1 of the holder. */
    volatile uint32_t holder;
} mutex_t;

#define MUTEX_INIT  { .holder = 0 }

static inline void mutex_init(mutex_t *m) {
    atomic_store_u32(&m->holder, 0);
}

/* Try once. Returns true if we got the lock. */
static inline bool mutex_trylock(mutex_t *m) {
    uint32_t self = smp_this_hart() + 1;
    return atomic_cas_u32(&m->holder, 0, self);
}

/* Spin until acquired. We `pause` (Zihintpause if available — RVVM
 * accepts the encoding even if the extension isn't advertised) on
 * each retry to hint a relaxed CPU loop. No `wfi`: wfi without an
 * MSIP wakeup wired here would deadlock a single contending hart. */
static inline void mutex_lock(mutex_t *m) {
    uint32_t self = smp_this_hart() + 1;
    while (!atomic_cas_u32(&m->holder, 0, self)) {
        /* Zihintpause: legal NOP that hints "I'm spinning" to the
         * pipeline. Encoded as `fence w, 0` per the ratified spec
         * — RVVM treats it as a no-op fence. Falls through harmlessly
         * on any RV implementation. */
        __asm__ volatile ("fence w, 0" ::: "memory");
    }
}

static inline void mutex_unlock(mutex_t *m) {
    /* Plain seq-cst store — release semantics on RVA's `aqrl`. */
    atomic_store_u32(&m->holder, 0);
}

/* Diagnostic: which hart currently holds the lock?  Returns -1 if
 * free, else the mhartid of the holder. Useful for "stuck" detection
 * in panics. Lockless read — value can change immediately after the
 * caller looks. */
static inline int mutex_holder(const mutex_t *m) {
    uint32_t v = atomic_load_u32((volatile uint32_t *)&m->holder);
    return v == 0 ? -1 : (int)(v - 1);
}
