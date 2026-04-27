/* Tiny PCI(e) ECAM enumerator + BAR placer + capability/MSI helpers.
 *
 * Just enough to: find a known vendor/device pair (or the Nth match,
 * or recurse through PCI-to-PCI bridges), read its BARs as RVVM
 * pre-placed them, walk the capability list, and configure MSI when
 * available. */

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
#define PCI_CFG_SECONDARY_BUS   0x19   /* type-1 header only */
#define PCI_CFG_SUBORDINATE_BUS 0x1A   /* type-1 header only */
#define PCI_CFG_CAP_PTR         0x34   /* offset of first capability, or 0 */
#define PCI_CFG_INTLINE         0x3C   /* RVVM auto-fills with PLIC source # */
#define PCI_CFG_INTPIN          0x3D

#define PCI_STATUS_CAP_LIST     0x0010 /* (status >> 16 in command_status) */

#define PCI_CMD_IO              0x0001
#define PCI_CMD_MEM             0x0002
#define PCI_CMD_BUS_MASTER      0x0004

/* Capability IDs we care about. Full list is in PCI Local Bus Spec
 * 3.0 §H. */
#define PCI_CAP_ID_PM           0x01   /* power management */
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_VENDOR       0x09
#define PCI_CAP_ID_PCIE         0x10
#define PCI_CAP_ID_MSIX         0x11

/* Initialise the PCI subsystem with the ECAM base address discovered
 * via FDT (compatible = "pci-host-ecam-generic"). Pass 0 to fall back
 * to RVVM's default 0x30000000. */
void pci_init(uintptr_t ecam_base);

/* Find the first function with a matching (vendor << 0 | device << 16)
 * config-space word. Walks bus 0 and recurses through any PCI-to-PCI
 * bridges into their secondary buses. RVVM doesn't ship bridges so the
 * recursive path never fires there, but the code stays correct on real
 * hardware. Fills `out` and returns true on success. */
bool pci_find_device(uint32_t vendor_device, pci_func_t *out);

/* Same as pci_find_device but returns the Nth match (0 = first). Useful
 * when more than one of a device is attached — e.g. multiple ATA
 * controllers, or two HID-over-I²C devices on the same bus. */
bool pci_find_device_nth(uint32_t vendor_device, uint32_t n, pci_func_t *out);

/* Size each populated BAR (write-1s probe) and place it at a fresh
 * address in the PCI MEM window via a bump allocator. Updates `out->bar[]`
 * and `out->bar_size[]`. Also writes COMMAND to enable MEM + BUS_MASTER. */
void pci_setup_bars(pci_func_t *func);

/* Walk the capability list looking for the requested ID. Returns the
 * config-space offset of the matching capability (always >= 0x40), or
 * 0 if absent or if the device doesn't advertise a capability list at
 * all. The status register's bit 4 (PCI_STATUS_CAP_LIST) gates the cap
 * pointer's validity per spec — devices that don't set it have no caps
 * even if cfg[0x34] reads non-zero. */
uint8_t pci_find_capability(const pci_func_t *func, uint8_t cap_id);

/* Configure the device's MSI capability to deliver to the given address
 * with the given data. Picks 1 message (MME=0) regardless of how many
 * the device requested. Honours 64-bit-address-capable devices.
 *
 * NOTE: in default RVVM (no -riscv_aia), MSI writes from PCI devices
 * are NOT routed to the PLIC — the host bridge converts them into
 * legacy INTx pin assertions instead. So configuring MSI on RVVM
 * doesn't change anything visible to firmware unless an IMSIC is
 * present at the configured address. Provided here for completeness
 * and for forward-compat with -riscv_aia machines.
 *
 * Returns true if the device has an MSI capability and was configured;
 * false if no MSI cap was found. */
bool pci_msi_configure(pci_func_t *func, uint64_t msi_addr, uint16_t msi_data);

/* Helpers for direct config-space access. */
static inline uint32_t pci_cfg_r32(const pci_func_t *f, uint32_t off) {
    return *(volatile uint32_t *)(f->cfg + off);
}
static inline uint16_t pci_cfg_r16(const pci_func_t *f, uint32_t off) {
    return *(volatile uint16_t *)(f->cfg + off);
}
static inline uint8_t pci_cfg_r8(const pci_func_t *f, uint32_t off) {
    return *(volatile uint8_t *)(f->cfg + off);
}
static inline void pci_cfg_w32(const pci_func_t *f, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(f->cfg + off) = v;
}
static inline void pci_cfg_w16(const pci_func_t *f, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(f->cfg + off) = v;
}
static inline void pci_cfg_w8(const pci_func_t *f, uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(f->cfg + off) = v;
}
