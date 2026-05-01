/* bench — microbenchmarks for the HAL's hot paths.
 *
 * Measures, in time-CSR ticks (RVVM = 100 ns / tick @ 10 MHz):
 *   1. memcpy 1 MiB × N
 *   2. memset 1 MiB × N
 *   3. memmove 1 MiB × N (forward, non-overlapping)
 *   4. trap entry — N self-IPIs through __trap_entry → trap_dispatch
 *
 * Used to compare the C / scalar baseline against the asm/slim variants
 * we're adding. Numbers are the wall-clock ticks elapsed between two
 * `time_now()` reads bracketing the loop, divided by the iteration count
 * to give a per-call figure.
 *
 * Caveats:
 *   - RVVM's `time` CSR isn't perfectly monotonic across host context
 *     switches; outliers happen. Repeated runs converge.
 *   - The first sample is always slower (icache / branch predictor
 *     warm-up). We discard a single warm-up pass per case. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "irq.h"
#include "plat.h"
#include "panic.h"
#include "gfx.h"
#include "rvvm.h"
#include <stdint.h>
#include <stddef.h>

extern char __bss_start[], __bss_end[];

#define BUF_BYTES   (1024 * 1024)        /* 1 MiB */
#define BUF_ITERS   16                   /* per measurement */
#define IRQ_ITERS   10000

/* Two 1 MiB buffers, page-aligned (the HAL string ops fast-path requires
 * src/dst share alignment; aligning to 4 KiB guarantees that). */
static uint8_t buf_a[BUF_BYTES] __attribute__((aligned(4096)));
static uint8_t buf_b[BUF_BYTES] __attribute__((aligned(4096)));

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int c, size_t n);
extern void *memmove(void *dst, const void *src, size_t n);

static uint64_t bench_memcpy(void) {
    /* warm */ memcpy(buf_a, buf_b, BUF_BYTES);
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < BUF_ITERS; i++) memcpy(buf_a, buf_b, BUF_BYTES);
    return time_now() - t0;
}

static uint64_t bench_memset(void) {
    /* warm */ memset(buf_a, 0xA5, BUF_BYTES);
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < BUF_ITERS; i++) memset(buf_a, 0xA5, BUF_BYTES);
    return time_now() - t0;
}

static uint64_t bench_memmove(void) {
    /* warm */ memmove(buf_a, buf_b, BUF_BYTES);
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < BUF_ITERS; i++) memmove(buf_a, buf_b, BUF_BYTES);
    return time_now() - t0;
}

/* gfx_rect bench: synthesise a 1024×256 framebuffer in BSS (= 1 MiB
 * @ 32bpp, same volume as the string benches so numbers compare). We
 * don't init a real GPU — just point a gfx_t at buf_a and pretend.
 * gfx_fill_row's three paths (memset, pair-pack, scalar) are exercised
 * by uniform-byte vs arbitrary pixel color. */
#define GFX_W   1024
#define GFX_H   256

static gfx_t fake_gfx;

static void fake_gfx_init(void) {
    fake_gfx.vram      = (uint32_t *)buf_a;
    fake_gfx.width     = GFX_W;
    fake_gfx.height    = GFX_H;
    fake_gfx.stride_px = GFX_W;
    fake_gfx.backend   = GFX_BACKEND_BOCHS;   /* anything != NONE */
    fake_gfx.format    = GFX_FMT_XRGB8888;
}

static uint64_t bench_gfx_uniform(void) {
    /* warm */ gfx_rect(&fake_gfx, 0, 0, GFX_W, GFX_H, 0x00000000);
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < BUF_ITERS; i++) {
        gfx_rect(&fake_gfx, 0, 0, GFX_W, GFX_H, 0x00000000);
    }
    return time_now() - t0;
}

static uint64_t bench_gfx_arbitrary(void) {
    /* 0x123456 has 4 distinct bytes — forces the pair-pack path. */
    /* warm */ gfx_rect(&fake_gfx, 0, 0, GFX_W, GFX_H, 0x00123456);
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < BUF_ITERS; i++) {
        gfx_rect(&fake_gfx, 0, 0, GFX_W, GFX_H, 0x00123456);
    }
    return time_now() - t0;
}

/* Self-IPI bench: pulse own MSIP via plat_ipi_send(self), the trap
 * fires on the next instruction (since global IE + soft-IE both on),
 * trap_dispatch acks via plat_ipi_ack and returns, mret comes back to
 * the loop. Each iteration = 1 round-trip through __trap_entry. */
static uint64_t bench_irq_entry(void) {
    uint32_t self = plat_this_hart();

    /* Enable global IE + soft IE — irq_global_enable handles MIE,
     * irq_init has set MEIE; we additionally need MSIE for software
     * interrupts. */
    irq_global_enable();
    uintptr_t bit = (1ULL << 3);   /* MIE_MSIE in M-mode build */
    __asm__ volatile ("csrs mie, %0" :: "r"(bit));

    plat_ipi_send(self);          /* warm */
    /* Wait for the warm-up trap to retire (mret will have happened by
     * now since we're sequential, but issue a serialising fence to be
     * extra sure). */
    __asm__ volatile ("fence rw,rw" ::: "memory");

    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < IRQ_ITERS; i++) {
        plat_ipi_send(self);
        /* The trap fires here (between plat_ipi_send and the next
         * loop branch) — the call into the dispatcher is the thing
         * we're timing. plat_ipi_ack inside the dispatcher clears
         * the source so the next iteration's send can re-pend. */
    }
    /* Drain any in-flight IRQ before stopping the timer. */
    __asm__ volatile ("fence rw,rw" ::: "memory");
    uint64_t t1 = time_now();

    irq_global_disable();
    return t1 - t0;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== rvvm-hal bench ===\n");
    uart_printf("hart=%u  fdt=%p  bss=%u bytes  buf=%u bytes×2\n\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start),
                (uint64_t)BUF_BYTES);

    fdt_t fdt;
    if (!fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uart_puts("FDT invalid; aborting.\n");
        hal_exit(1);
    }

    /* Initialise the platform from FDT before any timer / IRQ work. */
    uint32_t cpus_off  = fdt_find_node_named(&fdt, "cpus");
    uint32_t hz = 0;
    if (cpus_off != UINT32_MAX) fdt_node_prop_u32(&fdt, cpus_off, "timebase-frequency", &hz);
    uint32_t plic_off  = fdt_find_compatible(&fdt, "sifive,plic-1.0.0");
    uint32_t clint_off = fdt_find_compatible(&fdt, "sifive,clint0");
    uint64_t plic_at = 0, clint_at = 0;
    if (plic_off  != UINT32_MAX) fdt_node_reg64(&fdt, plic_off,  0, &plic_at,  NULL);
    if (clint_off != UINT32_MAX) fdt_node_reg64(&fdt, clint_off, 0, &clint_at, NULL);
    plat_init((uintptr_t)plic_at, (uintptr_t)clint_at);
    time_init(0, hz);
    irq_init((uintptr_t)plic_at);

    uart_printf("time: %u Hz (1 tick = %u ns)\n\n", time_hz(), 1000000000ULL / time_hz());

    /* Each measurement is iters × bytes (or simply iters for IRQ). */
    uint64_t t;

    t = bench_memcpy();
    uart_printf("memcpy  %u × %u B = %u ticks  (%u ns/iter, %u MiB/s)\n",
                (uint64_t)BUF_ITERS, (uint64_t)BUF_BYTES, t,
                (t * 100) / BUF_ITERS,
                (BUF_BYTES * (uint64_t)BUF_ITERS) / (t / 10));   /* B/(t/10) = bytes per microsecond = MB/s */

    t = bench_memset();
    uart_printf("memset  %u × %u B = %u ticks  (%u ns/iter, %u MiB/s)\n",
                (uint64_t)BUF_ITERS, (uint64_t)BUF_BYTES, t,
                (t * 100) / BUF_ITERS,
                (BUF_BYTES * (uint64_t)BUF_ITERS) / (t / 10));

    t = bench_memmove();
    uart_printf("memmove %u × %u B = %u ticks  (%u ns/iter, %u MiB/s)\n",
                (uint64_t)BUF_ITERS, (uint64_t)BUF_BYTES, t,
                (t * 100) / BUF_ITERS,
                (BUF_BYTES * (uint64_t)BUF_ITERS) / (t / 10));

    /* gfx_rect — same buffer volume as the string ops so MiB/s lines up. */
    fake_gfx_init();

    t = bench_gfx_uniform();
    uart_printf("\ngfx_rect %ux%u uniform-byte (memset path) %u × = %u ticks  (%u ns/iter, %u MiB/s)\n",
                (uint64_t)GFX_W, (uint64_t)GFX_H, (uint64_t)BUF_ITERS, t,
                (t * 100) / BUF_ITERS,
                (BUF_BYTES * (uint64_t)BUF_ITERS) / (t / 10));

    t = bench_gfx_arbitrary();
    uart_printf("gfx_rect %ux%u arbitrary  (pair-pack path) %u × = %u ticks  (%u ns/iter, %u MiB/s)\n",
                (uint64_t)GFX_W, (uint64_t)GFX_H, (uint64_t)BUF_ITERS, t,
                (t * 100) / BUF_ITERS,
                (BUF_BYTES * (uint64_t)BUF_ITERS) / (t / 10));

    t = bench_irq_entry();
    uart_printf("\nirq-entry %u × self-IPI = %u ticks  (%u ns/trap)\n",
                (uint64_t)IRQ_ITERS, t, (t * 100) / IRQ_ITERS);

    uart_puts("\nbench done.\n");
    hal_exit(0);
}
