/* Tiny PCI(e) ECAM enumerator + BAR placer. Just enough to find a
 * known vendor/device pair, size its BARs, allocate MMIO addresses
 * out of the PCI MEM window, and enable bus-master + memory access. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uintptr_t cfg;             /* base of this function's 4 KiB config space */
    /* BAR placement results — 0 if absent or unmapped. 64-bit BARs occupy
     * two slots; the high half lives in bar[i+1]. */
    uintptr_t bar[6];
    uint64_t  bar_size[6];
} pci_func_t;

/* Standard config-space registers (offsets within the 4 KiB region). */
#define PCI_CFG_VENDOR_DEVICE   0x00
#define PCI_CFG_COMMAND_STATUS  0x04
#define PCI_CFG_CLASS_REV       0x08
#define PCI_CFG_HEADER_TYPE     0x0E
#define PCI_CFG_BAR0            0x10
#define PCI_CFG_INTLINE         0x3C

#define PCI_CMD_IO              0x0001
#define PCI_CMD_MEM             0x0002
#define PCI_CMD_BUS_MASTER      0x0004

/* Initialise the PCI subsystem with the ECAM base address discovered
 * via FDT (compatible = "pci-host-ecam-generic"). Pass 0 to fall back
 * to RVVM's default 0x30000000. */
void pci_init(uintptr_t ecam_base);

/* Find the first function with a matching (vendor << 0 | device << 16)
 * config-space word. Fills `out` and returns true on success. */
bool pci_find_device(uint32_t vendor_device, pci_func_t *out);

/* Size each populated BAR (write-1s probe) and place it at a fresh
 * address in the PCI MEM window via a bump allocator. Updates `out->bar[]`
 * and `out->bar_size[]`. Also writes COMMAND to enable MEM + BUS_MASTER. */
void pci_setup_bars(pci_func_t *func);

/* Helpers for direct config-space access. */
static inline uint32_t pci_cfg_r32(const pci_func_t *f, uint32_t off) {
    return *(volatile uint32_t *)(f->cfg + off);
}
static inline uint16_t pci_cfg_r16(const pci_func_t *f, uint32_t off) {
    return *(volatile uint16_t *)(f->cfg + off);
}
static inline void pci_cfg_w32(const pci_func_t *f, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(f->cfg + off) = v;
}
static inline void pci_cfg_w16(const pci_func_t *f, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(f->cfg + off) = v;
}
