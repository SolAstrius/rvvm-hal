/* linpack — classic LU-decomposition FP benchmark (Linpack 100x100, double),
 * self-contained (no libm), to measure real-world FP throughput on RVVM.
 * Exercises the FP interpreter path with mixed mul/add/div/compare/abs across a
 * realistic memory-access pattern, not a tight single-op loop. Verifies the
 * solve residual, then times repeated factor+solve and reports MFLOPS.
 */
#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "panic.h"
#include <stdint.h>

#define N    100
#define LDA  (N + 1)
#define REPS 400

static double a[LDA * N], aorig[LDA * N], b[N], x[N];
static int    ipvt[N];

static inline double dabs(double v) { return v < 0.0 ? -v : v; }

/* Deterministic pseudo-random fill in [-0.5, 0.5). */
static uint32_t rng = 1325;
static double drand(void) {
    rng = rng * 1103515245u + 12345u;
    return (double)((rng >> 16) & 0x7fff) / 32768.0 - 0.5;
}

static void matgen(void) {
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++)
            aorig[i + j * LDA] = drand();
    /* b = A * (1,1,...,1) so the true solution is all-ones. */
    for (int i = 0; i < N; i++) {
        double s = 0.0;
        for (int j = 0; j < N; j++) s += aorig[i + j * LDA];
        b[i] = s;
    }
}

/* LU factorization with partial pivoting (LINPACK dgefa, column-major). */
static void dgefa(void) {
    for (int k = 0; k < N - 1; k++) {
        int l = k;
        for (int i = k + 1; i < N; i++)
            if (dabs(a[i + k * LDA]) > dabs(a[l + k * LDA])) l = i;
        ipvt[k] = l;
        double piv = a[l + k * LDA];
        double t = a[l + k * LDA]; a[l + k * LDA] = a[k + k * LDA]; a[k + k * LDA] = t;
        double m = -1.0 / piv;
        for (int i = k + 1; i < N; i++) a[i + k * LDA] *= m;
        for (int j = k + 1; j < N; j++) {
            t = a[l + j * LDA]; a[l + j * LDA] = a[k + j * LDA]; a[k + j * LDA] = t;
            for (int i = k + 1; i < N; i++)
                a[i + j * LDA] += t * a[i + k * LDA];
        }
    }
    ipvt[N - 1] = N - 1;
}

/* Solve A x = b using the factorization (LINPACK dgesl). */
static void dgesl(void) {
    for (int i = 0; i < N; i++) x[i] = b[i];
    for (int k = 0; k < N - 1; k++) {
        int l = ipvt[k];
        double t = x[l]; x[l] = x[k]; x[k] = t;
        for (int i = k + 1; i < N; i++) x[i] += t * a[i + k * LDA];
    }
    for (int kb = 0; kb < N; kb++) {
        int k = N - 1 - kb;
        x[k] /= a[k + k * LDA];
        double t = -x[k];
        for (int i = 0; i < k; i++) x[i] += t * a[i + k * LDA];
    }
}

static double residual(void) {
    double r = 0.0;
    for (int i = 0; i < N; i++) {
        double s = 0.0;
        for (int j = 0; j < N; j++) s += aorig[i + j * LDA] * x[j];
        double e = dabs(s - b[i]);
        if (e > r) r = e;
    }
    return r;
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
    uart_puts("\n=== linpack (100x100 LU, double) ===\n");

    matgen();
    /* correctness: factor+solve once, check residual */
    for (int i = 0; i < LDA * N; i++) a[i] = aorig[i];
    dgefa(); dgesl();
    double res = residual();
    /* residual scaled small (~1e-13); print as integer femto-units to avoid float printf */
    uart_printf("max residual ~ %u e-15\n", (uint32_t)(res * 1e15));

    uint64_t t0 = time_now();
    for (int r = 0; r < REPS; r++) {
        for (int i = 0; i < LDA * N; i++) a[i] = aorig[i];
        dgefa();
        dgesl();
    }
    uint64_t dt = time_now() - t0;

    /* FLOPs per factor+solve ~= 2/3 N^3 + 2 N^2 */
    uint64_t flop_per = (2ull * N * N * N) / 3ull + 2ull * N * N;
    uint64_t total_flop = flop_per * REPS;
    uint64_t us = dt / 10;                 /* ticks are 100 ns => /10 = microseconds */
    uint32_t mflops = us ? (uint32_t)(total_flop / us) : 0;

    uart_printf("reps=%u ticks=%u us=%u MFLOPS=%u\n",
                (unsigned)REPS, (uint32_t)dt, (uint32_t)us, mflops);
    uart_printf("LINPACK-DONE ticks=%u mflops=%u res_e15=%u\n",
                (uint32_t)dt, mflops, (uint32_t)(res * 1e15));
    hal_exit(res < 1e-9 ? 0 : 1);
}
