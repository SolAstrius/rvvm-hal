#include "fw_cfg.h"
#include "fdt.h"
#include "mmio.h"

#define FW_CFG_REG_DATA   0x00
#define FW_CFG_REG_SEL    0x08

static uintptr_t fw_cfg_base = 0;
static bool      fw_cfg_ok   = false;

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8)
         | ((v & 0x00FF0000U) >> 8)  | ((v & 0xFF000000U) >> 24);
}

/* fw-cfg's MMIO selector port is 16-bit BE; SBI/QEMU reads the host
 * byte order on the bus, so we byte-swap before writing. The data
 * port is a byte stream — single-byte reads work on every QEMU and
 * sidestep the wider-read endianness rules. */
void fw_cfg_select(uint16_t sel) {
    mmio_w16(fw_cfg_base + FW_CFG_REG_SEL, bswap16(sel));
}

void fw_cfg_read(void *dst, uint32_t len) {
    uint8_t *p = (uint8_t *)dst;
    while (len--) *p++ = mmio_r8(fw_cfg_base + FW_CFG_REG_DATA);
}

bool fw_cfg_init(uintptr_t base) {
    if (!base) return false;
    fw_cfg_base = base;
    char sig[4] = {0, 0, 0, 0};
    fw_cfg_select(FW_CFG_SIGNATURE);
    fw_cfg_read(sig, 4);
    fw_cfg_ok = (sig[0] == 'Q' && sig[1] == 'E' && sig[2] == 'M' && sig[3] == 'U');
    return fw_cfg_ok;
}

bool fw_cfg_init_fdt(const fdt_t *fdt) {
    uint32_t off = fdt_find_compatible(fdt, "qemu,fw-cfg-mmio");
    if (off == UINT32_MAX) return false;
    uint64_t at = 0, sz = 0;
    if (!fdt_node_reg64(fdt, off, 0, &at, &sz)) return false;
    return fw_cfg_init((uintptr_t)at);
}

bool fw_cfg_present(void) { return fw_cfg_ok; }

bool fw_cfg_find_file(const char *name, fw_cfg_file_t *out) {
    if (!fw_cfg_ok) return false;

    fw_cfg_select(FW_CFG_FILE_DIR);

    /* The directory leads with a u32 BE count. */
    uint8_t cb[4];
    fw_cfg_read(cb, 4);
    uint32_t count = ((uint32_t)cb[0] << 24) | ((uint32_t)cb[1] << 16)
                   | ((uint32_t)cb[2] << 8)  |  (uint32_t)cb[3];

    for (uint32_t i = 0; i < count; i++) {
        fw_cfg_file_t e;
        fw_cfg_read(&e, sizeof(e));

        /* size + select are BE on the wire. */
        e.size   = bswap32(e.size);
        e.select = bswap16(e.select);

        /* e.name is NUL-padded to 56 bytes. Equality test against the
         * caller's NUL-terminated name. */
        bool match = true;
        for (uint32_t k = 0; k < sizeof(e.name); k++) {
            char a = name[k], b = e.name[k];
            if (a != b) { match = false; break; }
            if (a == 0)   break;
        }
        if (match) {
            if (out) *out = e;
            return true;
        }
    }
    return false;
}
