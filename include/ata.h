/* Tiny PCI ATA PIO driver. Read-only, master-only, single LBA28 device.
 * Just enough to slurp a CHIP-8 ROM off a -ata-mounted file.
 *
 * RVVM's ATA emulation (src/devices/ata.c) is master-only PIO with
 * a few extras — it ignores BSY (operations are synchronous), supports
 * READ_SECTORS / WRITE_SECTORS / IDENTIFY plus a stubbed CHECK_POWER_MODE.
 * It also exposes BMDMA via BAR4 but we don't need it. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uintptr_t data;          /* BAR0 — task-file registers */
    uintptr_t ctl;           /* BAR1 — alternate-status / device-control */
    uint32_t  capacity;      /* total sectors (LBA28, max 256 MiB) */
    bool      present;
} ata_t;

/* Probe via PCI scan, set up BARs, send IDENTIFY. Returns true if a
 * disk is present and answered IDENTIFY. */
bool ata_init(ata_t *a);

/* Read up to `count` consecutive sectors (512 B each) starting at `lba`
 * into `buf`. Stops at the first sector whose READ_SECTORS returned
 * STATUS.ERR (e.g. seek past EOF). Returns the number of sectors actually
 * read (0..count). 0 means even sector 0 failed. */
uint32_t ata_read(ata_t *a, uint32_t lba, void *buf, uint32_t count);
