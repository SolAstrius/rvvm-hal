#include "uart.h"
#include "mmio.h"
#include "rvvm.h"

/* Set by uart_init(); used by every accessor. Defaults to RVVM's known
 * base so accidental access before init still talks to the right place. */
static uintptr_t uart_base = RVVM_UART_BASE;

#define UART_RBR    0x0
#define UART_THR    0x0
#define UART_DLL    0x0
#define UART_IER    0x1
#define UART_DLM    0x1
#define UART_IIR    0x2
#define UART_FCR    0x2
#define UART_LCR    0x3
#define UART_MCR    0x4
#define UART_LSR    0x5
#define UART_MSR    0x6
#define UART_SCR    0x7

#define LSR_DR      0x01
#define LSR_THRE    0x20
#define LSR_TEMT    0x40

#define LCR_8N1     0x03
#define LCR_DLAB    0x80

#define FCR_ENABLE  0x01
#define FCR_CLR_RX  0x02
#define FCR_CLR_TX  0x04
#define FCR_TRIG14  0xC0

#define MCR_DTR     0x01
#define MCR_RTS     0x02
#define MCR_OUT2    0x08

static inline uint8_t r(uint32_t off) { return mmio_r8(uart_base + off); }
static inline void    w(uint32_t off, uint8_t v) { mmio_w8(uart_base + off, v); }

void uart_init(uintptr_t base) {
    if (base) uart_base = base;

    w(UART_IER, 0x00);
    w(UART_LCR, LCR_DLAB);
    w(UART_DLL, 0x01);
    w(UART_DLM, 0x00);
    w(UART_LCR, LCR_8N1);
    w(UART_FCR, FCR_ENABLE | FCR_CLR_RX | FCR_CLR_TX | FCR_TRIG14);
    w(UART_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);
}

void uart_putc(char c) {
    while (!(r(UART_LSR) & LSR_THRE)) {}
    w(UART_THR, (uint8_t)c);
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_putc('\r');
        uart_putc(*s++);
    }
}

void uart_write(const void *buf, uint64_t len) {
    const char *p = buf;
    for (uint64_t i = 0; i < len; i++) uart_putc(p[i]);
}

int uart_getc_nb(void) {
    if (r(UART_LSR) & LSR_DR) {
        return (int)(uint8_t)r(UART_RBR);
    }
    return -1;
}

char uart_getc(void) {
    int c;
    while ((c = uart_getc_nb()) < 0) {}
    return (char)c;
}

static const char hex_digits[] = "0123456789abcdef";

void uart_put_hex8(uint8_t v) {
    uart_putc(hex_digits[(v >> 4) & 0xF]);
    uart_putc(hex_digits[v & 0xF]);
}

void uart_put_hex32(uint32_t v) {
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) uart_putc(hex_digits[(v >> i) & 0xF]);
}

void uart_put_hex64(uint64_t v) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc(hex_digits[(v >> i) & 0xF]);
}

void uart_put_udec(uint64_t v) {
    char buf[24];
    int  i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) uart_putc(buf[i]);
}

void uart_put_dec(int64_t v) {
    if (v < 0) { uart_putc('-'); v = -v; }
    uart_put_udec((uint64_t)v);
}

void uart_hexdump(const void *buf, uint64_t len) {
    const uint8_t *p = buf;
    for (uint64_t off = 0; off < len; off += 16) {
        uart_put_hex64((uint64_t)(uintptr_t)(p + off));
        uart_puts("  ");
        for (uint64_t i = 0; i < 16; i++) {
            if (off + i < len) { uart_put_hex8(p[off + i]); uart_putc(' '); }
            else                 uart_puts("   ");
            if (i == 7) uart_putc(' ');
        }
        uart_puts(" |");
        for (uint64_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = p[off + i];
            uart_putc((c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        uart_puts("|\n");
    }
}

#include <stdarg.h>

void uart_vprintf(const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') { uart_putc(*fmt++); continue; }
        fmt++;
        switch (*fmt++) {
        case 'c': uart_putc((char)va_arg(ap, int));               break;
        case 's': uart_puts(va_arg(ap, const char *));            break;
        case 'd': uart_put_dec(va_arg(ap, int64_t));              break;
        case 'u': uart_put_udec(va_arg(ap, uint64_t));            break;
        case 'x': uart_put_hex64(va_arg(ap, uint64_t));           break;
        case 'p': uart_put_hex64((uint64_t)va_arg(ap, void *));   break;
        case '%': uart_putc('%');                                 break;
        case 0:   return;
        default:  uart_putc('?');                                 break;
        }
    }
}

void uart_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uart_vprintf(fmt, ap);
    va_end(ap);
}
