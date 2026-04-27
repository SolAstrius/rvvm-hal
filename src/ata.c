#include "ata.h"
#include "pci.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

static inline uint8_t  R8 (uintptr_t base, uint32_t off) { return mmio_r8 (base + off); }
static inline uint16_t R16(uintptr_t base, uint32_t off) { return mmio_r16(base + off); }
static inline void     W8 (uintptr_t base, uint32_t off, uint8_t v) { mmio_w8(base + off, v); }

/* Wait until DRQ is set (data ready) or ERR is set. RVVM completes
 * commands synchronously inside the MMIO write callback so DRQ is
 * usually set the moment we return from the COMMAND write — the
 * spin is for portability. */
static bool wait_drq(uintptr_t data, int spin) {
    for (int i = 0; i < spin; i++) {
        uint8_t s = R8(data, ATA_REG_STATUS);
        if (s & ATA_STATUS_ERR) return false;
        if (s & ATA_STATUS_DRQ) return true;
    }
    return false;
}

bool ata_init(ata_t *a) {
    a->present = false;

    pci_func_t pf;
    if (!pci_find_device(RVVM_PCI_ID_ATA, &pf)) return false;
    pci_setup_bars(&pf);

    if (pf.bar[0] == 0 || pf.bar[1] == 0) {
        uart_printf("ata: missing BARs (bar0=%p bar1=%p)\n",
                    (void *)pf.bar[0], (void *)pf.bar[1]);
        return false;
    }
    a->data = pf.bar[0];
    a->ctl  = pf.bar[1];

    /* Select master, LBA mode (DEVICE = 0xE0). */
    W8(a->data, ATA_REG_DEVICE, 0xE0);

    /* IDENTIFY DEVICE — fills 256 16-bit words of drive info. */
    W8(a->data, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    if (!wait_drq(a->data, 100000)) {
        /* No drive present, or RVVM has no -ata mount. */
        return false;
    }

    /* Capacity in LBA28 sectors lives in words 60-61. */
    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = R16(a->data, ATA_REG_DATA);
    a->capacity = (uint32_t)id[60] | ((uint32_t)id[61] << 16);

    /* Drive name lives in words 27-46 (Model Number, 40 ASCII chars).
     * Bytes within each word are swapped per ATA spec — RVVM just
     * fills "SATA HDD" so it'll show up garbled but recognisable. */
    char model[41] = {0};
    for (int i = 0; i < 20; i++) {
        model[i * 2 + 0] = (id[27 + i] >> 8) & 0xFF;
        model[i * 2 + 1] =  id[27 + i]       & 0xFF;
    }
    /* Trim trailing spaces. */
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = 0;

    uart_printf("ata: %u:%u.%u  data=%p  ctl=%p  cap=%u sectors  \"%s\"\n",
                (uint64_t)pf.bus, (uint64_t)pf.dev, (uint64_t)pf.func,
                (void *)a->data, (void *)a->ctl,
                (uint64_t)a->capacity, model);

    a->present = true;
    return true;
}

/* Issue a single-sector READ_SECTORS for `lba`. Returns true if data
 * landed in `buf` (256 16-bit words = 512 bytes). */
static bool ata_read_one(ata_t *a, uint32_t lba, uint16_t *buf) {
    W8(a->data, ATA_REG_DEVICE, 0xE0 | ((lba >> 24) & 0xF));
    W8(a->data, ATA_REG_NSECT,  1);
    W8(a->data, ATA_REG_LBAL,   (uint8_t)(lba >>  0));
    W8(a->data, ATA_REG_LBAM,   (uint8_t)(lba >>  8));
    W8(a->data, ATA_REG_LBAH,   (uint8_t)(lba >> 16));
    W8(a->data, ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);

    if (!wait_drq(a->data, 100000)) return false;
    for (int i = 0; i < 256; i++) buf[i] = R16(a->data, ATA_REG_DATA);
    return true;
}

uint32_t ata_read(ata_t *a, uint32_t lba, void *buf, uint32_t count) {
    if (!a->present || count == 0) return 0;
    uint16_t *p = (uint16_t *)buf;
    uint32_t  done = 0;
    /* Single-sector reads in a loop. Multi-sector READ_SECTORS would
     * be faster but RVVM short-reads (e.g. for files smaller than the
     * requested span) become an STATUS.ERR mid-stream that's awkward
     * to recover from. One-sector-at-a-time stops cleanly. */
    while (done < count) {
        if (!ata_read_one(a, lba + done, p)) break;
        p    += 256;
        done += 1;
    }
    return done;
}
