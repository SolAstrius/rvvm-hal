/* ui-hello — menu / dialog smoke test (UART backend).
 *
 *   make
 *   make run
 *
 * Boots, mounts an exFAT image as drive 0 if one's attached, then
 * presents a top-level menu:
 *
 *     === ui-hello ===
 *
 *     > Pick a file
 *       Confirm dialog
 *       Show message
 *       Quit
 *
 * Use ↑↓ to navigate, Enter to select, Esc to back out.
 *
 * The pick-file branch uses FatFs to list /. */

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

extern char __bss_start[], __bss_end[];

static nvme_t disk;
static ui_t   ui;

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
    if (fdt_init(&fdt, (const void *)(uintptr_t)fdt_addr)) {
        uint32_t off = fdt_find_compatible(&fdt, "ns16550a");
        uint64_t at = 0;
        fdt_node_reg64(&fdt, off, 0, &at, NULL);
        uart_init((uintptr_t)at);
        off = fdt_find_compatible(&fdt, "pci-host-ecam-generic");
        if (off != UINT32_MAX) {
            fdt_node_reg64(&fdt, off, 0, &at, NULL);
            pci_init((uintptr_t)at);
        }
    }

    bool fs_ok = false;
    if (nvme_init(&disk) && fs_mount(&disk, "0:")) {
        fs_ok = true;
    }

    ui_init_uart(&ui);

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
