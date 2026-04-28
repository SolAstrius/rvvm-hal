/* picolibc-hello — smoke test for the vendored picolibc.
 *
 * Calls real <stdio.h> printf from picolibc, which routes through
 * the FILE put callback wired in src/picolibc_hooks.c (uart_putc).
 * Also exercises malloc/strtol/qsort to make sure those link too.
 *
 *   nix develop ../.. -c make picolibc-min        # one-time
 *   nix develop ../.. -c make
 *   make run
 *
 * Expected output: a printf line, a malloc demonstration, an
 * strtol parse, and a sorted array. If anything fails to link,
 * picolibc's hooks (or the consumer LDFLAGS) are wrong. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "uart.h"
#include "fdt.h"

extern char __bss_start[], __bss_end[];

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== picolibc-hello ===\n");

    fdt_t fdt;
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t off = fdt_find_compatible(&fdt, "ns16550a");
        uint64_t at = 0;
        fdt_node_reg64(&fdt, off, 0, &at, NULL);
        uart_init((uintptr_t)at);
    }

    /* 1. Real printf via picolibc's tinystdio. */
    printf("[picolibc] printf works. hart=%llu, fdt=%p, bss=%lu bytes\n",
           (unsigned long long)hartid,
           (void *)(uintptr_t)fdt_addr,
           (unsigned long)(__bss_end - __bss_start));

    /* 2. malloc/free via _sbrk → bump pool. */
    char *buf = (char *)malloc(64);
    if (buf) {
        strcpy(buf, "[picolibc] malloc + strcpy work");
        printf("%s (len=%zu)\n", buf, strlen(buf));
        free(buf);
    } else {
        printf("[picolibc] malloc FAILED\n");
    }

    /* 3. strtol parses a string. */
    const char *s = "0xDEADBEEF rest";
    char *end = NULL;
    long v = strtol(s, &end, 0);
    printf("[picolibc] strtol(\"%s\", 0) = 0x%lx, end=\"%s\"\n", s, v, end);

    /* 4. qsort sorts an array — exercises function-pointer callback. */
    int arr[] = { 9, 3, 7, 1, 8, 2, 6, 5, 4, 0 };
    qsort(arr, 10, sizeof(int), cmp_int);
    printf("[picolibc] qsort: ");
    for (int i = 0; i < 10; i++) printf("%d ", arr[i]);
    printf("\n");

    printf("\ndone.\n");
    for (;;) __asm__ volatile ("wfi");
}
