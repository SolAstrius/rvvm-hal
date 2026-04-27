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
#include "bochs.h"
#include "i2c.h"
#include "hid.h"
#include "ata.h"
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

    /* 3. Bochs Display. */
    bochs_t bd;
    bool gfx = bochs_init(&bd, 640, 400);
    if (gfx) {
        /* Colour test pattern: vertical RGBW bars. */
        uint32_t colours[4] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};
        for (uint32_t y = 0; y < 400; y++) {
            for (uint32_t x = 0; x < 640; x++) {
                bd.vram[y * 640 + x] = colours[(x * 4) / 640];
            }
        }
        uart_puts("bochs: 640x400 framebuffer up, 4-bar test pattern drawn.\n");
    } else {
        uart_puts("bochs: not present (start RVVM with -bochs_display).\n");
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

    /* 5. HID keyboard polling, echo to UART. */
    hid_keyboard_t kb;
    hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);
    uart_puts("\nNow polling HID keyboard (focus the GUI window). Ctrl-A x to quit RVVM.\n");

    uint64_t deadline = time_now() + RVVM_TICKS_PER_FRAME;
    for (;;) {
        hid_kb_poll(&kb, on_key, NULL);
        time_busy_until(deadline);
        deadline += RVVM_TICKS_PER_FRAME;
    }
}
