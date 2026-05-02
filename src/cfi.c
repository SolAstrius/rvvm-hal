#include "cfi.h"
#include "fdt.h"
#include "mmio.h"

/* CFI query commands. Same opcodes for Intel (cfi01) and AMD (cfi02)
 * — the difference is in program/erase, which we don't implement. */
#define CFI_CMD_READ_ARRAY  0xFF
#define CFI_CMD_QUERY       0x98

/* CFI query-mode table offsets (within the table; NOT MMIO offsets —
 * MMIO offset = table_idx * bank_width). */
#define CFI_QRY_Q           0x10
#define CFI_QRY_R           0x11
#define CFI_QRY_Y           0x12
#define CFI_QRY_CMDSET_LO   0x13
#define CFI_QRY_CMDSET_HI   0x14
#define CFI_QRY_DEV_SIZE_N  0x27   /* size = 1 << this_byte */

static inline uint8_t cfi_read_table(const cfi_t *f, uint32_t idx) {
    /* Each table entry is replicated across the bus width (Intel CFI
     * §7.4): on a 32-bit bus we get the byte in the low byte of the
     * read, with high bytes zero or replicated. mmio_r8 on the right
     * MMIO offset gives us the underlying byte cleanly. */
    return mmio_r8(f->base + (uintptr_t)idx * f->bank_width);
}

static inline void cfi_cmd(const cfi_t *f, uint8_t cmd) {
    /* Commands ignore the address on Intel cfi01 ("any address");
     * write at offset 0 with a bus-width access so the write reaches
     * the device's command register intact. */
    if (f->bank_width == 4)      mmio_w32(f->base, cmd);
    else if (f->bank_width == 2) mmio_w16(f->base, cmd);
    else                          mmio_w8 (f->base, cmd);
}

bool cfi_init(cfi_t *out, uintptr_t base, uint32_t bank_width) {
    out->base       = base;
    out->bank_width = bank_width ? bank_width : 4;
    out->size_bytes = 0;
    out->cmdset     = 0;
    out->ok         = false;
    if (!base) return false;

    cfi_cmd(out, CFI_CMD_QUERY);

    /* QRY signature confirms a real, queryable flash. An unmapped /
     * unbacked region reads 0xFF and won't satisfy this. */
    if (cfi_read_table(out, CFI_QRY_Q) != 'Q' ||
        cfi_read_table(out, CFI_QRY_R) != 'R' ||
        cfi_read_table(out, CFI_QRY_Y) != 'Y') {
        cfi_cmd(out, CFI_CMD_READ_ARRAY);
        return false;
    }

    out->cmdset = (uint16_t)cfi_read_table(out, CFI_QRY_CMDSET_LO)
                | ((uint16_t)cfi_read_table(out, CFI_QRY_CMDSET_HI) << 8);
    uint8_t size_n = cfi_read_table(out, CFI_QRY_DEV_SIZE_N);
    out->size_bytes = (size_n < 32) ? (1U << size_n) : 0;
    out->ok = true;

    cfi_cmd(out, CFI_CMD_READ_ARRAY);
    return true;
}

bool cfi_init_fdt(cfi_t *out, const fdt_t *fdt) {
    uint32_t off = fdt_find_compatible(fdt, "cfi-flash");
    if (off == UINT32_MAX) {
        out->ok = false;
        return false;
    }
    uint64_t at = 0, sz = 0;
    if (!fdt_node_reg64(fdt, off, 0, &at, &sz)) return false;

    uint32_t bw = 4;
    fdt_node_prop_u32(fdt, off, "bank-width", &bw);
    if (!cfi_init(out, (uintptr_t)at, bw)) return false;

    /* CFI's device-size byte reports a single die's capacity. The
     * full bank as visible to MMIO is the FDT `reg` size — usually
     * larger because the bank is built from multiple paralleled dies
     * (QEMU virt: two 16 MiB dies → 32 MiB region). Trust the FDT. */
    if (sz > out->size_bytes) out->size_bytes = (uint32_t)sz;
    return true;
}

uint32_t cfi_read(const cfi_t *flash, uint32_t offset,
                  void *dst, uint32_t len) {
    if (!flash->ok || offset >= flash->size_bytes) return 0;
    if (offset + len > flash->size_bytes) len = flash->size_bytes - offset;

    /* Read-array mode is just memory; loop bytes (cheap, simple, and
     * the consumer rarely reads megabytes from CFI flash). */
    const volatile uint8_t *src = (const volatile uint8_t *)
                                  (flash->base + offset);
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < len; i++) p[i] = src[i];
    return len;
}
