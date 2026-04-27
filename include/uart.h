/* NS16550A driver — polling-only, blocking TX, non-blocking RX.
 *
 * RVVM places ns16550a at 0x10000000 by default (src/devices/ns16550a.h).
 * The chardev_term backend routes UART TX to host stdout and RX to host
 * stdin in raw mode — so puts() lands in the terminal RVVM was launched
 * from, and a host keystroke shows up at uart_getc_nb() on the next read.
 *
 * RVVM ignores the baud divisor, FIFO control, modem control. We program
 * them anyway so this driver is portable to a real 16550A. */

#pragma once
#include <stdint.h>
#include <stdarg.h>

/* Initialise the UART driver. Pass the MMIO base address discovered
 * via FDT (compatible = "ns16550a"). If `base` is 0, falls back to
 * RVVM's default of 0x10000000. */
void uart_init(uintptr_t base);

/* Output. */
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const void *buf, uint64_t len);

/* Number formatting. */
void uart_put_hex64(uint64_t val);   /* 0x{16 hex digits} */
void uart_put_hex32(uint32_t val);   /* 0x{8 hex digits} */
void uart_put_hex8 (uint8_t  val);   /* 2 hex digits, no prefix */
void uart_put_udec(uint64_t val);
void uart_put_dec (int64_t  val);

/* Hex dump: addr/16-byte rows, ASCII gutter. */
void uart_hexdump(const void *buf, uint64_t len);

/* Tiny printf. Format directives: %c %s %d %u %x %p %% — no width specs,
 * no precision, no padding flags. %d/%u are 64-bit; cast at call site. */
void uart_printf(const char *fmt, ...);
void uart_vprintf(const char *fmt, va_list ap);

/* Input. */
int  uart_getc_nb(void);   /* -1 if no char available */
char uart_getc   (void);   /* blocks */
