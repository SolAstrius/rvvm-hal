/* Picolibc integration hooks.
 *
 * Picolibc's tinystdio uses a per-FILE put/get callback rather than
 * the POSIX `_write`/`_read`/`_lseek` tree, so the integration is
 * three lines plus a couple of process-level stubs:
 *
 *   uart_putc_io     character sink for our single FILE
 *   stdin/stdout/stderr  point at that one FILE
 *   _exit            picolibc calls this from atexit-style paths and
 *                    on assert failure; we wfi forever
 *
 * Heap is provided by link.ld via __heap_start / __heap_end symbols,
 * consumed by picolibc's own sbrk (vendor/picolibc/libc/misc/picosbrk.c).
 * We don't supply our own sbrk — picolibc's has proper errno + bounds
 * handling and we benefit from upstream maintenance.
 *
 * Compiled only when HAL_PICOLIBC is defined (Makefile gate). The
 * .o is otherwise excluded from libhal.a so default builds stay
 * picolibc-free. */

#ifdef HAL_PICOLIBC

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "uart.h"

/* tinystdio's FILE has a put callback — `int putc(char c, FILE *)`.
 * Return value is the byte written or EOF on error; we never fail. */
static int uart_putc_io(char c, FILE *f) {
    (void)f;
    uart_putc(c);
    return (unsigned char)c;
}

/* Read direction. Blocks until a byte arrives — picolibc's fgets,
 * scanf, getchar, and friends expect blocking semantics. Returns the
 * byte plus a \r→\n normalisation (most terminals deliver CR for the
 * Enter key, but the C-string convention is \n line endings). For
 * non-blocking reads call rvvm-hal's uart_getc_nb directly. */
static int uart_getc_io(FILE *f) {
    (void)f;
    char c = uart_getc();
    int r = (c == '\r') ? '\n' : (unsigned char)c;
    return r;
}

static FILE __stdio_uart =
    FDEV_SETUP_STREAM(uart_putc_io, uart_getc_io, NULL,
                      _FDEV_SETUP_RW);

FILE *const stdin  = &__stdio_uart;
FILE *const stdout = &__stdio_uart;
FILE *const stderr = &__stdio_uart;

/* Process termination. Picolibc's assert macro lands here. */
__attribute__((noreturn))
void _exit(int status) {
    (void)status;
    for (;;) __asm__ volatile ("wfi");
}

#endif /* HAL_PICOLIBC */
