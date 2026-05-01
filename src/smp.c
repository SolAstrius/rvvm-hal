#include "smp.h"
#include "fdt.h"
#include "plat.h"
#include "uart.h"
#include <stddef.h>
#include <stdint.h>

/* Per-hart command slot — primary writes (fn, arg), secondary reads
 * after MSIP wake. `volatile` forces actual loads/stores; cross-hart
 * visibility is enforced with explicit `fence rw,rw` on both sides
 * around the running flag. */
static volatile struct {
    smp_entry_t fn;
    void       *arg;
} smp_cmd[SMP_MAX_HARTS];

/* Set by primary in smp_start, cleared by the secondary's dispatcher
 * after the entry returns. smp_wait spins on this. */
static volatile uint8_t smp_running[SMP_MAX_HARTS];

static uint32_t hart_count_v = 1;        /* primary always present */

/* String compare for FDT property values (no <string.h> in freestanding,
 * and the existing helper in fdt.c is private to that TU). */
static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

/* Skip over a node's name string + trailing NUL, padded to 4 bytes.
 * Returns the offset of the first token after the name. Mirrors
 * fdt.c's static skip_name — duplicated here because that helper is
 * private to fdt.c. */
static uint32_t name_end(const fdt_t *fdt, uint32_t off) {
    uint32_t p = off + 4;
    while (fdt->struct_blob[p]) p++;
    p++;
    return (p + 3) & ~3U;
}

/* Walk the entire structure block, counting nodes whose `device_type`
 * property is "cpu". RVVM tags every CPU node that way (rvvm.c:171),
 * and no other node uses the same string. Counting via property is
 * simpler than depth-walking /cpus children. */
static uint32_t count_cpu_nodes(const fdt_t *fdt) {
    uint32_t off = 0;
    uint32_t count = 0;
    while (off < fdt->struct_size) {
        uint32_t tok = fdt_read_be32(fdt->struct_blob, off);
        switch (tok) {
        case FDT_BEGIN_NODE: {
            uint32_t len = 0;
            const char *dt = fdt_node_prop(fdt, off, "device_type", &len);
            if (dt && str_eq(dt, "cpu")) count++;
            off = name_end(fdt, off);   /* descend into body */
            break;
        }
        case FDT_END_NODE:
        case FDT_NOP:
            off += 4; break;
        case FDT_PROP: {
            uint32_t plen = fdt_read_be32(fdt->struct_blob, off + 4);
            off = off + 12 + ((plen + 3) & ~3U);
            break;
        }
        case FDT_END:
        default:
            return count;
        }
    }
    return count;
}

uint32_t smp_this_hart(void) {
    return plat_this_hart();
}

uint32_t smp_hart_count(void) { return hart_count_v; }

void smp_init(const fdt_t *fdt) {
    if (!fdt) return;

    /* Discover CLINT base and forward to the platform layer.
     * Secondaries' initial wake path in start.S uses the hardcoded
     * RVVM default (0x02000000) — smp_start (via plat_ipi_send) uses
     * whatever plat_init was given, so non-default machines still
     * work as long as their CLINT is at the default at the time of
     * the very first wake. */
    uint32_t clint_off = fdt_find_compatible(fdt, "sifive,clint0");
    if (clint_off != UINT32_MAX) {
        uint64_t at = 0;
        if (fdt_node_reg64(fdt, clint_off, 0, &at, NULL) && at) {
            plat_init(0, (uintptr_t)at);
        }
    }

    uint32_t n = count_cpu_nodes(fdt);
    if (n == 0) n = 1;
    if (n > SMP_MAX_HARTS) {
        uart_printf("smp: %u harts in FDT, capped at %u (link.ld stack limit)\n",
                    (uint64_t)n, (uint64_t)SMP_MAX_HARTS);
        n = SMP_MAX_HARTS;
    }
    hart_count_v = n;

    /* Bring secondaries into the park loop. M+CLINT no-op (start.S
     * already parked them at reset); S+SBI ecalls each one in. After
     * this loop, every hart in [1, n) is reachable via plat_ipi_send. */
    uint32_t self = plat_this_hart();
    for (uint32_t h = 0; h < n; h++) {
        if (h == self) continue;
        plat_hart_bringup_park(h);
    }
}

bool smp_start(uint32_t hartid, smp_entry_t fn, void *arg) {
    if (hartid == 0) return false;                 /* primary is us */
    if (hartid >= hart_count_v) return false;      /* not present */
    if (smp_running[hartid]) return false;         /* already busy */
    if (!fn) return false;

    smp_cmd[hartid].fn  = fn;
    smp_cmd[hartid].arg = arg;
    smp_running[hartid] = 1;
    /* plat_ipi_send issues a fence before delivering the wake, so the
     * command + running flag are visible to the secondary before it
     * observes the IPI. */
    plat_ipi_send(hartid);
    return true;
}

bool smp_is_running(uint32_t hartid) {
    if (hartid >= SMP_MAX_HARTS) return false;
    return smp_running[hartid] != 0;
}

void smp_wait(uint32_t hartid) {
    /* Tight busy-loop. The secondary clears smp_running with a fence,
     * so primary will see the write. We don't wfi here because the
     * primary doesn't have an MSIP/timer wake source wired by default
     * — adding one would entangle this with irq.c. Callers that want
     * to do useful work while waiting should poll smp_is_running()
     * themselves. */
    while (smp_is_running(hartid)) {
        __asm__ volatile ("" ::: "memory");
    }
}

/* Called from start.S after a secondary wakes from wfi and clears its
 * own MSIP. Reads the queued (fn, arg), runs it, marks the hart idle.
 * Hartid is passed in — must NOT call smp_this_hart() here, the asm
 * already paid for the csrr. */
void __smp_secondary_dispatch(uint64_t hartid) {
    if (hartid >= SMP_MAX_HARTS) return;
    smp_entry_t fn  = smp_cmd[hartid].fn;
    void       *arg = smp_cmd[hartid].arg;
    if (fn) fn(hartid, arg);
    /* Order: all stores by fn() must be visible before primary sees
     * us idle (otherwise smp_wait could return while the entry's
     * results were still in this hart's store buffer). */
    __asm__ volatile ("fence rw,rw" ::: "memory");
    smp_running[hartid] = 0;
}
