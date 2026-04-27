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

bool pci_find_device(uint32_t vendor_device, pci_func_t *out) {
    /* Crawl bus 0 — RVVM puts auto-attached endpoints here directly off
     * the host bridge. Multifunction handling: read header type byte
     * from func 0; bit 7 set means scan funcs 1-7 too. */
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            uintptr_t cfg = ecam_addr(0, dev, func);
            uint32_t  id  = *(volatile uint32_t *)(cfg + PCI_CFG_VENDOR_DEVICE);
            if (id == 0xFFFFFFFFU) continue;
            if (id == vendor_device) {
                out->bus  = 0;
                out->dev  = dev;
                out->func = func;
                out->cfg  = cfg;
                for (int i = 0; i < 6; i++) {
                    out->bar[i]      = 0;
                    out->bar_size[i] = 0;
                }
                return true;
            }
            if (func == 0) {
                uint8_t hdr = *(volatile uint8_t *)(cfg + PCI_CFG_HEADER_TYPE);
                if (!(hdr & 0x80)) break;
            }
        }
    }
    return false;
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
