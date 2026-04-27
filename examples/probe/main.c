/* probe — demonstrates every rvvm-hal driver in one ~150-line main.c.
 *
 * Boots, walks the FDT, lists every device discovered, brings up the
 * Bochs framebuffer if available, dumps ATA disk info, and echoes HID
 * keyboard events to the UART. No architecture emulation — pure HAL
 * walkthrough. */

#include "uart.h"
#include "time.h"
#include "fdt.h"
#include "pci.h"
#include "gfx.h"
#include "i2c.h"
#include "hid.h"
#include "ata.h"
#include "irq.h"
#include "rvvm.h"

extern char __bss_start[], __bss_end[];

static void list_compat(const fdt_t *fdt, const char *compat) {
    uint32_t off = fdt_find_compatible(fdt, compat);
    if (off == UINT32_MAX) { uart_printf("  (none)  %s\n", compat); return; }
    uint64_t addr = 0, size = 0;
    fdt_node_reg64(fdt, off, 0, &addr, &size);
    uart_printf("  %s @ %p  size=%x\n",
                compat, (void *)(uintptr_t)addr, size);
}

static void on_key(uint8_t usage, bool pressed, void *ctx) {
    (void)ctx;
    uart_printf("  hid: usage=0x%x %s\n", (uint64_t)usage,
                pressed ? "DOWN" : "up");
}

/* UART RX IRQ handler — drains the FIFO so LSR.DR drops and the PLIC
 * sees the source de-assert. Echoes each byte back to the host with a
 * tag so it's obvious the path is interrupt-driven. */
static void on_uart_rx(uint32_t source, void *ctx) {
    (void)ctx;
    int c;
    while ((c = uart_getc_nb()) >= 0) {
        uart_printf("[irq #%u] got byte 0x%x ('%c')\n",
                    (uint64_t)source, (uint64_t)c,
                    (c >= 0x20 && c < 0x7F) ? (uint64_t)c : (uint64_t)'.');
    }
}

/* Look up the `interrupts` u32 property on a node. RVVM emits it as a
 * single big-endian cell pointing at a PLIC source number. Returns 0
 * if absent or the node lacks an `interrupts` property. */
static uint32_t fdt_node_irq(const fdt_t *fdt, uint32_t node_off) {
    uint32_t len = 0;
    const void *p = fdt_node_prop(fdt, node_off, "interrupts", &len);
    if (!p || len < 4) return 0;
    return fdt_read_be32(p, 0);
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== rvvm-hal probe ===\n");
    uart_printf("hart=%u  fdt=%p  bss=%u bytes\n\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

    /* 1. FDT walk. */
    fdt_t fdt;
    if (!fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uart_puts("FDT invalid; aborting.\n");
        for (;;) __asm__ volatile ("wfi");
    }
    uart_puts("Devices discovered via FDT:\n");
    list_compat(&fdt, "ns16550a");
    list_compat(&fdt, "opencores,i2c-ocores");
    list_compat(&fdt, "pci-host-ecam-generic");
    list_compat(&fdt, "sifive,plic-1.0.0");
    list_compat(&fdt, "sifive,clint0");
    list_compat(&fdt, "google,goldfish-rtc");
    list_compat(&fdt, "syscon");
    uart_putc('\n');

    /* 2. Re-init drivers with discovered addresses. */
    uint32_t u_off = fdt_find_compatible(&fdt, "ns16550a");
    uint32_t p_off = fdt_find_compatible(&fdt, "pci-host-ecam-generic");
    uint32_t i_off = fdt_find_compatible(&fdt, "opencores,i2c-ocores");
    uint64_t u_at = 0, p_at = 0, i_at = 0;
    fdt_node_reg64(&fdt, u_off, 0, &u_at, NULL);
    fdt_node_reg64(&fdt, p_off, 0, &p_at, NULL);
    fdt_node_reg64(&fdt, i_off, 0, &i_at, NULL);
    uart_init((uintptr_t)u_at);
    pci_init((uintptr_t)p_at);
    i2c_init((uintptr_t)i_at);

    /* 3. Graphics — auto-selects bochs (preferred) or simple-framebuffer. */
    gfx_t g;
    bool have_gfx = gfx_init_fdt(&g, &fdt, 640, 400);
    if (have_gfx) {
        const char *backend = (g.backend == GFX_BACKEND_BOCHS)    ? "bochs"
                            : (g.backend == GFX_BACKEND_SIMPLEFB) ? "simplefb"
                                                                  : "?";
        uart_printf("gfx: %s backend, %ux%u, format=%s\n",
                    backend, (uint64_t)g.width, (uint64_t)g.height,
                    g.format == GFX_FMT_XRGB8888 ? "XRGB8888" : "XBGR8888");
        /* Colour test pattern: vertical RGBW bars in XRGB8888. */
        uint32_t colours[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};
        for (int i = 0; i < 4; i++) {
            uint32_t bw = g.width / 4;
            gfx_rect(&g, i * bw, 0, bw, g.height, colours[i]);
        }
        uart_puts("gfx: 4-bar test pattern drawn.\n");
    } else {
        uart_puts("gfx: no display backend (try -bochs_display or -res WxH).\n");
    }

    /* 4. ATA. */
    ata_t disk;
    if (ata_init(&disk)) {
        uint8_t buf[16];
        if (ata_read(&disk, 0, buf, 1) == 1) {
            uart_puts("ata: first 16 bytes of sector 0:\n  ");
            for (int i = 0; i < 16; i++) uart_printf("%x ", (uint64_t)buf[i]);
            uart_putc('\n');
        }
    } else {
        uart_puts("ata: no disk attached (start RVVM with -ata <file>).\n");
    }

    /* 5. IRQ subsystem — wire UART RX-data through the PLIC. */
    uint32_t plic_off = fdt_find_compatible(&fdt, "sifive,plic-1.0.0");
    uint64_t plic_at  = 0;
    if (plic_off != UINT32_MAX) fdt_node_reg64(&fdt, plic_off, 0, &plic_at, NULL);
    irq_init((uintptr_t)plic_at);

    uint32_t uart_irq = fdt_node_irq(&fdt, u_off);
    if (uart_irq) {
        uart_printf("\nirq: PLIC @ %p; UART source line = %u\n",
                    (void *)(uintptr_t)plic_at, (uint64_t)uart_irq);
        irq_register(uart_irq, on_uart_rx, NULL);
        irq_set_priority(uart_irq, 3);
        irq_enable(uart_irq);
        uart_irq_rx_enable(true);
        irq_global_enable();
        uart_puts("irq: type into the host terminal — bytes echo via interrupt path.\n");
    } else {
        uart_puts("\nirq: UART has no `interrupts` FDT property; skipping demo\n");
    }

    /* 6. HID keyboard polling, echo to UART. */
    hid_keyboard_t kb;
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);
    uart_puts("\nNow polling HID keyboard (focus the GUI window). Ctrl-A x to quit RVVM.\n");

    uint64_t deadline = time_now() + RVVM_TICKS_PER_FRAME;
    uint32_t last_irq_count = 0;
    for (;;) {
        hid_kb_poll(&kb, on_key, NULL);

        /* Sanity check — print a heartbeat every ~3 s with the IRQ
         * counter so it's visible that interrupts are flowing. */
        uint32_t now_count = irq_count_total();
        if (now_count != last_irq_count
            && (now_count - last_irq_count) >= 4) {
            uart_printf("(irq counter = %u)\n", (uint64_t)now_count);
            last_irq_count = now_count;
        }

        time_busy_until(deadline);
        deadline += RVVM_TICKS_PER_FRAME;
    }
}
