/* fs-hello — exFAT-on-NVMe smoke test.
 *
 * Boots, mounts NVMe controller 0 as FatFs drive "0:", reads
 * /hello.txt if it exists, appends a boot tag, writes back.
 *
 *   make fs-image     # one-time: create a 16 MiB exFAT NVMe image
 *                     # with /hello.txt seeded
 *   make
 *   make run          # runs RVVM with -nvme exfat.img attached
 *
 * Each subsequent run appends a "boot N at TICK" line, so re-mounting
 * the disk image on the host shows the file growing. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "uart.h"
#include "fdt.h"
#include "pci.h"
#include "time.h"
#include "rvvm.h"
#include "nvme.h"
#include "fs.h"
#include "ff.h"

extern char __bss_start[], __bss_end[];

static nvme_t disk;

void kmain(uint64_t hartid, uint64_t fdt_addr) {
    uart_init(0);
    uart_puts("\n=== fs-hello (exFAT on NVMe via FatFs) ===\n");
    uart_printf("hart=%u  fdt=%p  bss=%u bytes\n",
                hartid, (void *)(uintptr_t)fdt_addr,
                (uint64_t)(__bss_end - __bss_start));

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

    if (!nvme_init(&disk)) {
        printf("no NVMe controller found — start RVVM with -nvme exfat.img\n");
        for (;;) __asm__ volatile ("wfi");
    }

    if (!fs_mount(&disk, "0:")) {
        printf("fs_mount failed — is the disk formatted (mkfs.exfat)?\n");
        for (;;) __asm__ volatile ("wfi");
    }
    printf("mounted 0:/  (%lu LBAs, %lu MiB)\n",
           (unsigned long)disk.num_lbas,
           (unsigned long)((disk.num_lbas * NVME_LBA_SIZE) >> 20));

    /* Read existing contents (if any). */
    {
        FIL fp;
        FRESULT r = f_open(&fp, "0:/hello.txt", FA_READ);
        if (r == FR_OK) {
            char buf[256];
            UINT got = 0;
            f_read(&fp, buf, sizeof(buf) - 1, &got);
            buf[got] = 0;
            printf("--- /hello.txt (current contents, %u bytes) ---\n%s",
                   (unsigned)got, buf);
            if (got > 0 && buf[got - 1] != '\n') puts("");
            puts("--- end of file ---");
            f_close(&fp);
        } else {
            printf("/hello.txt not found (FRESULT=%d) — will create.\n",
                   (int)r);
        }
    }

    /* Append a boot tag. */
    {
        FIL fp;
        FRESULT r = f_open(&fp, "0:/hello.txt", FA_OPEN_APPEND | FA_WRITE);
        if (r != FR_OK) {
            printf("f_open(append) failed: FRESULT=%d\n", (int)r);
        } else {
            char line[64];
            int n = snprintf(line, sizeof(line),
                             "boot at mtime=%llu\n",
                             (unsigned long long)time_now());
            UINT put = 0;
            f_write(&fp, line, (UINT)n, &put);
            f_close(&fp);
            printf("appended %u bytes: %s", put, line);
        }
    }

    /* Re-read to confirm the write took. */
    {
        FIL fp;
        if (f_open(&fp, "0:/hello.txt", FA_READ) == FR_OK) {
            FSIZE_t sz = f_size(&fp);
            printf("file is now %lu bytes\n", (unsigned long)sz);
            f_close(&fp);
        }
    }

    fs_unmount("0:");
    puts("\ndone — mount exfat.img on the host to see the appended line.");
    for (;;) __asm__ volatile ("wfi");
}
