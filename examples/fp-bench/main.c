/* fp-bench — FP OP-FP throughput microbench for RVVM, to measure the cost of
 * the interpreter's FP handler (riscv_emulate_f_opc_op) with/without a patch.
 *
 * Default rounding (frm=RNE) — the path real workloads hit. Each loop body is
 * inline asm so the host compiler can't fold/vectorize it; values stay near 1.0
 * to avoid denormal/inf timing artifacts. Reports time-CSR ticks (100 ns each).
 */
#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "panic.h"
#include <stdint.h>

#define ITERS 40000000u

static uint64_t bench_addmul(void) {
    double a = 1.0, b = 1.0, c = 0.0, d = 0.0;
    double ku = 1.0000000001, kd = 0.9999999999, t = 1e-12;
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < ITERS; i++) {
        __asm__ volatile(
            "fmul.d %0,%0,%4\n\t"
            "fmul.d %1,%1,%5\n\t"
            "fadd.d %2,%2,%6\n\t"
            "fsub.d %3,%3,%6\n\t"
            : "+f"(a), "+f"(b), "+f"(c), "+f"(d)
            : "f"(ku), "f"(kd), "f"(t));
    }
    __asm__ volatile("" :: "f"(a), "f"(b), "f"(c), "f"(d));
    return time_now() - t0;
}

static uint64_t bench_div(void) {
    double a = 1.0, b = 1.0; double ku = 1.0000000001, kd = 0.9999999999;
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < ITERS; i++) {
        __asm__ volatile("fdiv.d %0,%0,%2\n\tfdiv.d %1,%1,%3\n\t"
            : "+f"(a), "+f"(b) : "f"(ku), "f"(kd));
    }
    __asm__ volatile("" :: "f"(a), "f"(b));
    return time_now() - t0;
}

static uint64_t bench_sqrt(void) {
    double a = 1.5, b = 2.5;
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < ITERS; i++) {
        __asm__ volatile("fsqrt.d %0,%0\n\tfsqrt.d %1,%1\n\t" : "+f"(a), "+f"(b));
        a += 0.5; b += 0.5;  /* keep > 1 so it doesn't collapse to 1.0 */
    }
    __asm__ volatile("" :: "f"(a), "f"(b));
    return time_now() - t0;
}

static uint64_t bench_fma(void) {
    double a = 1.0, b = 1.0, c = 0.0, d = 0.0;
    double ku = 1.0000000001, kd = 0.9999999999, t = 1e-12;
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < ITERS; i++) {
        __asm__ volatile(
            "fmadd.d  %0,%0,%4,%6\n\t"
            "fmsub.d  %1,%1,%5,%6\n\t"
            "fnmadd.d %2,%2,%4,%6\n\t"
            "fnmsub.d %3,%3,%5,%6\n\t"
            : "+f"(a), "+f"(b), "+f"(c), "+f"(d)
            : "f"(ku), "f"(kd), "f"(t));
    }
    __asm__ volatile("" :: "f"(a), "f"(b), "f"(c), "f"(d));
    return time_now() - t0;
}

static uint64_t bench_addmul_s(void) {
    float a = 1.0f, b = 1.0f, c = 0.0f, d = 0.0f;
    float ku = 1.0000001f, kd = 0.9999999f, t = 1e-9f;
    uint64_t t0 = time_now();
    for (uint32_t i = 0; i < ITERS; i++) {
        __asm__ volatile(
            "fmul.s %0,%0,%4\n\t"
            "fmul.s %1,%1,%5\n\t"
            "fadd.s %2,%2,%6\n\t"
            "fsub.s %3,%3,%6\n\t"
            : "+f"(a), "+f"(b), "+f"(c), "+f"(d)
            : "f"(ku), "f"(kd), "f"(t));
    }
    __asm__ volatile("" :: "f"(a), "f"(b), "f"(c), "f"(d));
    return time_now() - t0;
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    fdt_t fdt;
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t off = fdt_find_compatible(&fdt, "ns16550a");
        uint64_t at = 0;
        if (fdt_node_reg64(&fdt, off, 0, &at, NULL)) uart_init((uintptr_t)at);
    }
    (void)hartid;
    uart_puts("\n=== fp-bench ===\n");
    /* warm-up */ (void)bench_addmul();
    uint64_t am = bench_addmul();
    uint64_t dv = bench_div();
    uint64_t sq = bench_sqrt();
    uint64_t fm = bench_fma();
    uint64_t as = bench_addmul_s();
    uart_printf("addmul %u ops: %u ticks\n", ITERS * 4u, (uint32_t)am);
    uart_printf("div    %u ops: %u ticks\n", ITERS * 2u, (uint32_t)dv);
    uart_printf("sqrt   %u ops: %u ticks\n", ITERS * 2u, (uint32_t)sq);
    uart_printf("fma    %u ops: %u ticks\n", ITERS * 4u, (uint32_t)fm);
    uart_printf("addmul.s %u ops: %u ticks\n", ITERS * 4u, (uint32_t)as);
    uart_printf("FP-BENCH-DONE addmul=%u div=%u sqrt=%u fma=%u addmuls=%u\n",
                (uint32_t)am, (uint32_t)dv, (uint32_t)sq, (uint32_t)fm, (uint32_t)as);
    hal_exit(0);
}
