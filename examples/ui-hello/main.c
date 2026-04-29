/* ui-hello — menu / dialog smoke test, dual UART + GFX backend.
 *
 *   make
 *   make run            # headless (UART only)
 *   make run-gui        # Bochs framebuffer + HID keyboard
 *
 * Boots, mounts an exFAT image as drive 0 if one's attached, brings
 * up the Bochs framebuffer if RVVM was started with -bochs_display,
 * and registers BOTH UART and GFX backends with ui. The same menu
 * appears on the framebuffer window AND in the host terminal; either
 * input source advances it.
 *
 *     === ui-hello ===
 *
 *     > Pick a file
 *       Confirm dialog
 *       Show message
 *       Quit
 *
 * Use ↑↓ (GUI) or arrow keys in the host terminal to navigate,
 * Enter to select, Esc to back out. */

#include <stdio.h>
#include <string.h>

#include "uart.h"
#include "fdt.h"
#include "pci.h"
#include "time.h"
#include "rvvm.h"
#include "nvme.h"
#include "fs.h"
#include "ui.h"
#include "gfx.h"
#include "gfx_text.h"
#include "i2c.h"
#include "hid.h"

#include "font_8x8.h"

extern char __bss_start[], __bss_end[];

static nvme_t disk;
static ui_t   ui;

/* GFX state — 40×25 character grid on the 640×400 Bochs default.
 * 8×8 font glyphs scaled ×2 = 16×16 px per cell; 40×16 = 640 px
 * wide, 25×16 = 400 px tall — fills the framebuffer exactly. */
#define GRID_COLS 40
#define GRID_ROWS 25
static uint8_t  grid_chars[GRID_COLS * GRID_ROWS];
static gfx_t    g;
static gfx_text_t text = {
    .cols = GRID_COLS, .rows = GRID_ROWS,
    .cell_h = 8, .scale = 2,
    .font = font_8x8_ascii, .chars = grid_chars,
    .attrs = NULL,
    .fg = 0x00FFFFFFu, .bg = 0x00000000u,
};
static hid_keyboard_t kb;

static void demo_pick_file(void) {
    char path[128];
    if (ui_pick_file(&ui, "/", NULL, path, sizeof(path))) {
        char msg[160];
        snprintf(msg, sizeof(msg), "picked: %s", path);
        ui_message(&ui, msg);
    }
}

static void demo_confirm(void) {
    if (ui_confirm(&ui, "Reboot now?")) {
        ui_message(&ui, "(would reboot)");
    } else {
        ui_message(&ui, "(cancelled)");
    }
}

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);

    fdt_t fdt;
    bool have_fdt = fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr);
    if (have_fdt) {
        uint32_t off = fdt_find_compatible(&fdt, "ns16550a");
        uint64_t at = 0;
        fdt_node_reg64(&fdt, off, 0, &at, NULL);
        uart_init((uintptr_t)at);
        off = fdt_find_compatible(&fdt, "pci-host-ecam-generic");
        if (off != UINT32_MAX) {
            fdt_node_reg64(&fdt, off, 0, &at, NULL);
            pci_init((uintptr_t)at);
        }
        /* I²C — needed for HID keyboard. */
        off = fdt_find_compatible(&fdt, "opencores,i2c-ocores");
        if (off != UINT32_MAX) {
            fdt_node_reg64(&fdt, off, 0, &at, NULL);
            i2c_init((uintptr_t)at);
        }
    }

    bool fs_ok = false;
    if (nvme_init(&disk) && fs_mount(&disk, "0:")) {
        fs_ok = true;
    }

    /* Always wire UART backend. */
    ui_init_uart(&ui);

    /* Try to bring up GFX backend. gfx_init_fdt prefers Bochs
     * Display when present (RVVM started with -bochs_display) and
     * falls back to simple-framebuffer (-res WxH). Returns false
     * headless. */
    bool have_gfx = false;
    if (have_fdt && gfx_init_fdt(&g, &fdt, 640, 400)) {
        gfx_fill(&g, 0x00000000u);            /* black background */
        gfx_text_clear(&text, 0);
        hid_kb_init(&kb, RVVM_I2C_HID_KEYBOARD);
        ui_init_gfx(&ui, &g, &text, &kb);
        have_gfx = true;
    }

    uart_printf("ui-hello: backends = %s%s%s, fs=%s\n",
                ui.uart_enabled ? "UART " : "",
                have_gfx ? "GFX " : "",
                (!ui.uart_enabled && !have_gfx) ? "(none!)" : "",
                fs_ok ? "mounted" : "no FS");

    /* Top-level menu. The "Pick a file" item disables itself when
     * no FS is mounted. */
    ui_item_t items[] = {
        { .label = "Pick a file",     .id = 1, .disabled = !fs_ok,
          .hint = fs_ok ? NULL : "no FS attached" },
        { .label = "Confirm dialog",  .id = 2 },
        { .label = "Show message",    .id = 3 },
        { .label = "Quit",            .id = 4 },
    };
    ui_menu_t menu = {
        .title = "ui-hello", .items = items,
        .count = sizeof(items)/sizeof(items[0]),
        .selected = 0,
    };

    for (;;) {
        int picked = ui_menu_run(&ui, &menu);
        switch (picked) {
        case 1: demo_pick_file(); break;
        case 2: demo_confirm();   break;
        case 3: ui_message(&ui, "rvvm-hal v1.0 — menu primitives demo"); break;
        case 4:
        case -1:
            uart_puts("\033[2J\033[Hbye.\n");
            for (;;) __asm__ volatile ("wfi");
        }
    }
}
