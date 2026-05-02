/* CFI parallel-NOR flash — JEDEC Common Flash Interface.
 *
 * Present on QEMU's `virt` machine (compatible = "cfi-flash") as two
 * 32 MiB banks at 0x20000000 / 0x22000000, normally consumed by
 * EDK2/U-Boot for UEFI variable storage. Absent on RVVM. Real-world
 * use case: same MMIO pattern on most SoC boards (i.MX, MPC, etc),
 * so the same driver works on real hardware reachable via QEMU.
 *
 * Today this is a read-only driver: it identifies the chip via the
 * CFI query window, parses size, and exposes direct memory-mapped
 * reads. Erase + program (Intel command set 0x0001, AMD 0x0002) are
 * not implemented — for the firmware-storage use case the host's
 * `-drive if=pflash,...` loads content at startup, so guests rarely
 * need to write to it.
 *
 * Quick geometry refresher:
 *   - `bank_width` is the host bus width (1/2/4 bytes). QEMU virt =4.
 *   - In CFI query mode, table entry N is read at offset N*bank_width.
 *   - The table starts with "QRY" at entries 0x10..0x12.
 *   - Default state is read-array: plain memory-mapped flash content.
 *     We always restore that mode on init so reads work afterwards.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"

typedef struct {
    uintptr_t base;
    uint32_t  bank_width;     /* 1, 2, or 4 — from FDT `bank-width` */
    uint32_t  size_bytes;     /* total device size, parsed from CFI */
    uint16_t  cmdset;         /* 0x0001=Intel, 0x0002=AMD; informational */
    bool      ok;
} cfi_t;

/* Discover via FDT (compatible = "cfi-flash"). Probes the chip via a
 * CFI query, fills `out`, returns true on success. Returns false on
 * any host without a CFI flash node (e.g. RVVM), and on hosts where
 * the node is present but no `-drive if=pflash` was supplied (the
 * unbacked region returns 0xFF and the QRY signature won't show). */
bool cfi_init_fdt(cfi_t *out, const fdt_t *fdt);

/* Initialise from a known base + bank_width without FDT. */
bool cfi_init(cfi_t *out, uintptr_t base, uint32_t bank_width);

/* Read `len` bytes starting at `offset` within the flash. The chip is
 * left in read-array mode after init, so this is a plain memcpy from
 * MMIO. Returns the number of bytes actually copied (clipped to
 * device size). */
uint32_t cfi_read(const cfi_t *flash, uint32_t offset,
                  void *dst, uint32_t len);
