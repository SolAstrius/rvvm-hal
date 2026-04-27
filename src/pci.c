#include "pci.h"
#include "rvvm.h"
#include "uart.h"

/* ECAM base — set by pci_init(). Defaults to RVVM's hardcode in case
 * pci_init wasn't called (e.g. no FDT). */
static uintptr_t ecam_base = RVVM_PCI_ECAM_BASE;

void pci_init(uintptr_t base) {
    if (base) ecam_base = base;
}

/* ECAM address layout: ECAM_BASE | bus<<20 | dev<<15 | func<<12 | reg.
 * 256 buses × 32 devices × 8 functions × 4 KiB. */
static inline uintptr_t ecam_addr(uint8_t bus, uint8_t dev, uint8_t func) {
    return ecam_base
         | ((uintptr_t)bus  << 20)
         | ((uintptr_t)dev  << 15)
         | ((uintptr_t)func << 12);
}

/* Fill `out` for a function known to be at (bus, dev, func). Caller has
 * already verified the vendor/device match. */
static void fill_func(pci_func_t *out, uint8_t bus, uint8_t dev, uint8_t func, uintptr_t cfg) {
    out->bus  = bus;
    out->dev  = dev;
    out->func = func;
    out->cfg  = cfg;
    for (int i = 0; i < 6; i++) {
        out->bar[i]      = 0;
        out->bar_size[i] = 0;
    }
}

/* Recursive bus scan. RVVM doesn't ship PCI-to-PCI bridges (every
 * endpoint sits on bus 0), but a bridge would be advertised via header
 * type 0x01 — when we see one, we read its secondary bus number from
 * config offset 0x19 and recurse. Subordinate (0x1A) bounds the
 * subtree; we don't enforce it because we always stop on the first
 * match anyway. */
static bool scan_bus(uint8_t bus, uint32_t vendor_device, uint32_t target_n,
                     uint32_t *seen, pci_func_t *out)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        bool multifunc = false;
        for (uint8_t func = 0; func < 8; func++) {
            uintptr_t cfg = ecam_addr(bus, dev, func);
            uint32_t  id  = *(volatile uint32_t *)(cfg + PCI_CFG_VENDOR_DEVICE);
            if (id == 0xFFFFFFFFU) {
                if (func == 0) break;       /* whole slot empty */
                continue;                   /* gap inside multifunc */
            }

            if (id == vendor_device) {
                if (*seen == target_n) {
                    fill_func(out, bus, dev, func, cfg);
                    return true;
                }
                (*seen)++;
            }

            uint8_t hdr = *(volatile uint8_t *)(cfg + PCI_CFG_HEADER_TYPE);
            if (func == 0) multifunc = (hdr & 0x80) != 0;

            /* PCI-to-PCI bridge — recurse into its secondary bus. */
            if ((hdr & 0x7F) == 0x01) {
                uint8_t sec = *(volatile uint8_t *)(cfg + PCI_CFG_SECONDARY_BUS);
                if (sec != 0 && sec != bus) {
                    if (scan_bus(sec, vendor_device, target_n, seen, out)) {
                        return true;
                    }
                }
            }

            if (func == 0 && !multifunc) break;
        }
    }
    return false;
}

bool pci_find_device(uint32_t vendor_device, pci_func_t *out) {
    return pci_find_device_nth(vendor_device, 0, out);
}

bool pci_find_device_nth(uint32_t vendor_device, uint32_t n, pci_func_t *out) {
    uint32_t seen = 0;
    return scan_bus(0, vendor_device, n, &seen, out);
}

void pci_setup_bars(pci_func_t *func) {
    /* RVVM auto-assigns BAR addresses at device-attach time
     * (src/devices/pci-bus.c:457 pci_assign_mmio_addr) — the conventional
     * "write 0xFFFFFFFF, read back the size mask, write desired address"
     * dance doesn't apply here, because the MMIO regions are already
     * registered at fixed places. So we just read the BARs as RVVM left
     * them and trust the address.
     *
     * Quirk: RVVM's read returns bar->addr OR'd with PCI_BAR_64_BIT
     * (0x4) for 64-bit BARs and PCI_BAR_PREFETCH (0x8) for sizes ≥256MiB.
     * Mask those out. RVVM never sets the IO-space flag (bit 0) for
     * non-IO BARs, so it survives untouched.
     *
     * Size is unknown without a destructive probe; callers that need it
     * should hardcode from rvvm.h device constants. */
    for (int i = 0; i < 6; i++) {
        uint32_t off = PCI_CFG_BAR0 + (uint32_t)i * 4;
        uint32_t lo  = pci_cfg_r32(func, off);
        if (lo == 0) continue;

        bool is_io  = (lo & 0x1);
        bool is_64  = (!is_io && ((lo & 0x6) == 0x4));
        uint32_t mask = is_io ? 0x3U : 0xFU;

        uint64_t addr = lo & ~mask;
        if (is_64) {
            uint32_t hi = pci_cfg_r32(func, off + 4);
            addr |= ((uint64_t)hi) << 32;
        }
        func->bar[i] = (uintptr_t)addr;
        if (is_64) i++;
    }

    /* Make sure MEM + BUS_MASTER are set. RVVM defaults the command
     * register to PCI_CMD_DEFAULT (IO|MEM|BUS_MASTER) at attach time
     * (src/devices/pci-bus.c:489), so this is usually a no-op. */
    uint16_t cmd = pci_cfg_r16(func, PCI_CFG_COMMAND_STATUS);
    pci_cfg_w16(func, PCI_CFG_COMMAND_STATUS, cmd | PCI_CMD_MEM | PCI_CMD_BUS_MASTER);
}

uint8_t pci_find_capability(const pci_func_t *func, uint8_t cap_id) {
    /* Spec §6.7: cap pointer is only valid if status bit 4 says so. */
    uint32_t cmdsts = pci_cfg_r32(func, PCI_CFG_COMMAND_STATUS);
    uint16_t status = (uint16_t)(cmdsts >> 16);
    if (!(status & PCI_STATUS_CAP_LIST)) return 0;

    uint8_t off = pci_cfg_r8(func, PCI_CFG_CAP_PTR) & 0xFC;  /* low 2 bits reserved */
    /* Walk with a hop budget to defend against malformed cyclic lists. */
    for (int hops = 0; off != 0 && hops < 48; hops++) {
        uint8_t id   = pci_cfg_r8(func, off + 0);
        uint8_t next = pci_cfg_r8(func, off + 1) & 0xFC;
        if (id == cap_id) return off;
        if (next == off) break;   /* defensive */
        off = next;
    }
    return 0;
}

/* MSI capability layout (PCI Local Bus Spec 3.0 §6.8.1):
 *   off 0   ID = 0x05
 *   off 1   next pointer
 *   off 2   message control (16-bit)
 *             bit  0    : enable
 *             bits 3:1  : multiple-message capable (log2 N requested)
 *             bits 6:4  : multiple-message enable  (log2 N allocated)
 *             bit  7    : 64-bit address capable
 *             bit  8    : per-vector masking capable
 *   off 4   message address (low or full 32-bit)
 *   off 8   message address high  (only if bit 7 of msg_ctl)
 *   off 8 or C   message data (16-bit)
 *   off C/10     mask bits      (only if bit 8 of msg_ctl)
 *   off 10/14    pending bits   (only if bit 8 of msg_ctl) */
bool pci_msi_configure(pci_func_t *func, uint64_t msi_addr, uint16_t msi_data) {
    uint8_t cap = pci_find_capability(func, PCI_CAP_ID_MSI);
    if (!cap) return false;

    uint16_t msg_ctl  = pci_cfg_r16(func, cap + 2);
    bool     addr_64  = (msg_ctl & (1u << 7)) != 0;

    /* Force MME=0 (1 message), enable. Preserve the device's reported
     * MMC and the maskable bit so writes don't trip RW1C-style traps. */
    uint16_t new_ctl = (msg_ctl & ~0x70u) | 0x0001u;

    pci_cfg_w32(func, cap + 4, (uint32_t)(msi_addr & 0xFFFFFFFFu));
    if (addr_64) {
        pci_cfg_w32(func, cap + 8,  (uint32_t)(msi_addr >> 32));
        pci_cfg_w16(func, cap + 12, msi_data);
    } else {
        if ((msi_addr >> 32) != 0) {
            uart_puts("pci_msi_configure: device only supports 32-bit address\n");
        }
        pci_cfg_w16(func, cap + 8,  msi_data);
    }
    pci_cfg_w16(func, cap + 2, new_ctl);
    return true;
}
