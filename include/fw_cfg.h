/* QEMU fw-cfg — the host's "firmware config" channel.
 *
 * Present on QEMU's `virt` machine (compatible = "qemu,fw-cfg-mmio")
 * and absent on RVVM. Lets a guest pull boot info — RAM size, CPU
 * count, kernel/initrd locations, ACPI tables, host UUID, and
 * arbitrary host-supplied files (`-fw_cfg name=opt/foo,file=...`).
 *
 * MMIO layout (24 bytes total, all big-endian wire format):
 *   0x00  data port    — 8 bytes wide; reads stream out the selected
 *                        entry one byte at a time, internal pointer
 *                        advances per access.
 *   0x08  selector     — 16-bit. Writing a key (BE) selects which
 *                        entry the data port returns next.
 *   0x10  DMA control  — 8-byte BE descriptor pointer (skipped here;
 *                        byte-stream reads are fast enough for the
 *                        small structures fw-cfg exposes).
 *
 * Wire format and selector list: docs/specs/fw_cfg.rst in qemu.git.
 *
 * On hosts without fw-cfg (RVVM, real hardware), `fw_cfg_init*`
 * returns false and `fw_cfg_present()` stays false; callers can
 * branch on that and fall back to whatever they were doing.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"

/* Standard selector keys (qemu hw/nvram/fw_cfg.h). */
#define FW_CFG_SIGNATURE     0x0000   /* 4 bytes "QEMU" */
#define FW_CFG_ID            0x0001   /* u32: bit 0 = traditional, bit 1 = DMA */
#define FW_CFG_UUID          0x0002   /* 16 bytes */
#define FW_CFG_RAM_SIZE      0x0003   /* u32, little-endian (legacy) */
#define FW_CFG_NB_CPUS       0x0005   /* u16 */
#define FW_CFG_KERNEL_ADDR   0x0007
#define FW_CFG_KERNEL_SIZE   0x0008
#define FW_CFG_KERNEL_CMDLINE 0x000B
#define FW_CFG_INITRD_ADDR   0x000C
#define FW_CFG_INITRD_SIZE   0x000D
#define FW_CFG_FILE_DIR      0x0019   /* directory of named files */

/* One entry in the FW_CFG_FILE_DIR listing. `size` and `select` are
 * stored big-endian on the wire; the public copy you get back from
 * fw_cfg_find_file() has them swapped to host order. */
typedef struct {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char     name[56];   /* path-style, e.g. "etc/system-states" */
} fw_cfg_file_t;

/* Initialise. Returns true if a "QEMU" signature is present at the
 * given base. `base` may be 0 to no-op. Idempotent. */
bool fw_cfg_init(uintptr_t base);

/* Discover via FDT (compatible = "qemu,fw-cfg-mmio"). Returns true
 * on success, false if the node is absent (i.e. running on RVVM or
 * any non-QEMU host). */
bool fw_cfg_init_fdt(const fdt_t *fdt);

/* Was a working fw-cfg detected? */
bool fw_cfg_present(void);

/* Select an entry by key. Subsequent fw_cfg_read() calls stream that
 * entry's bytes from the start. */
void fw_cfg_select(uint16_t selector);

/* Read `len` bytes from the currently-selected entry's byte stream. */
void fw_cfg_read(void *dst, uint32_t len);

/* Walk the file directory and return the entry matching `name`
 * (NUL-terminated, exact match). On hit: fills *out (with size and
 * select in host byte order) and returns true. */
bool fw_cfg_find_file(const char *name, fw_cfg_file_t *out);
