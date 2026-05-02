/* virtio-mmio transport — modern (v1.0+) only.
 *
 * QEMU's `virt` machine exposes 8 virtio-mmio slots at
 * 0x10001000..0x10008000 (FDT compatible = "virtio,mmio"). Each slot
 * is empty unless the host attaches a device via
 * `-device virtio-<type>-device`. A populated slot's MagicValue reads
 * "virt" and Version is 2 for modern transport (1 = legacy).
 *
 * What this layer covers:
 *   - Slot probe: read MagicValue/Version/DeviceID/VendorID at the
 *     known register offsets.
 *   - Init handshake: RESET → ACK → DRIVER → feature negotiate →
 *     FEATURES_OK → queue setup → DRIVER_OK.
 *   - Split-virtqueue plumbing: descriptor table, avail ring, used
 *     ring, push/pop, notify, polling on used.idx.
 *
 * What it does NOT cover (deliberately):
 *   - Legacy virtio v0.9.5 — register layout is different and we
 *     don't have a host that needs it.
 *   - The packed virtqueue (VIRTIO_F_RING_PACKED) — split is fine.
 *   - DMA-coherent mappings / non-identity address translation —
 *     bare-metal, MMU off, virtual==physical.
 *
 * Spec: virtio-v1.2 §4.2 (MMIO), §2.7 (split virtqueue), §3.1 (init).
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"

/* MMIO register offsets (modern, virtio-v1.2 §4.2.2). */
#define VIRTIO_MMIO_MAGIC            0x000   /* RO  "virt" = 0x74726976 */
#define VIRTIO_MMIO_VERSION          0x004   /* RO  1=legacy, 2=modern */
#define VIRTIO_MMIO_DEVICE_ID        0x008   /* RO  0=invalid, see VIRTIO_ID_* */
#define VIRTIO_MMIO_VENDOR_ID        0x00c   /* RO  0x554d4551 = "QEMU" */
#define VIRTIO_MMIO_DEVICE_FEAT      0x010   /* RO  32 bits per FEAT_SEL */
#define VIRTIO_MMIO_DEVICE_FEAT_SEL  0x014   /* WO */
#define VIRTIO_MMIO_DRIVER_FEAT      0x020   /* WO */
#define VIRTIO_MMIO_DRIVER_FEAT_SEL  0x024   /* WO */
#define VIRTIO_MMIO_QUEUE_SEL        0x030   /* WO */
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034   /* RO */
#define VIRTIO_MMIO_QUEUE_NUM        0x038   /* WO */
#define VIRTIO_MMIO_QUEUE_READY      0x044   /* RW  1=ready */
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050   /* WO  write queue index */
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060   /* RO */
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064   /* WO */
#define VIRTIO_MMIO_STATUS           0x070   /* RW */
#define VIRTIO_MMIO_QUEUE_DESC_LO    0x080   /* WO  desc table phys addr */
#define VIRTIO_MMIO_QUEUE_DESC_HI    0x084   /* WO */
#define VIRTIO_MMIO_QUEUE_DRIVER_LO  0x090   /* WO  avail ring */
#define VIRTIO_MMIO_QUEUE_DRIVER_HI  0x094
#define VIRTIO_MMIO_QUEUE_DEVICE_LO  0x0a0   /* WO  used ring */
#define VIRTIO_MMIO_QUEUE_DEVICE_HI  0x0a4
#define VIRTIO_MMIO_CONFIG_GEN       0x0fc
#define VIRTIO_MMIO_CONFIG           0x100   /* device-specific config */

#define VIRTIO_MAGIC_VALUE           0x74726976U   /* "virt" little-endian */

/* Status bits (§2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE    0x01
#define VIRTIO_STATUS_DRIVER         0x02
#define VIRTIO_STATUS_DRIVER_OK      0x04
#define VIRTIO_STATUS_FEATURES_OK    0x08
#define VIRTIO_STATUS_NEEDS_RESET    0x40
#define VIRTIO_STATUS_FAILED         0x80

/* Standard device IDs we recognise. The full list is in the spec
 * §5; add more here as we add drivers. */
#define VIRTIO_ID_INVALID            0
#define VIRTIO_ID_NET                1
#define VIRTIO_ID_BLOCK              2
#define VIRTIO_ID_CONSOLE            3
#define VIRTIO_ID_RNG                4
#define VIRTIO_ID_GPU                16
#define VIRTIO_ID_INPUT              18

/* Universal feature bits (§6). */
#define VIRTIO_F_VERSION_1_BIT       32   /* must-accept for modern */

/* Descriptor flags (§2.7.5). */
#define VIRTQ_DESC_F_NEXT            1
#define VIRTQ_DESC_F_WRITE           2     /* device writes (input) */
#define VIRTQ_DESC_F_INDIRECT        4

/* Split virtqueue, packed for the device's view. We tag every member
 * with __attribute__((packed)) so the compiler doesn't insert padding
 * — the device DMA-reads these structs at exactly these offsets. */
struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];   /* size depends on queue length */
} __attribute__((packed));

struct virtq_used_elem {
    uint32_t id;       /* head descriptor index */
    uint32_t len;      /* bytes written by the device */
} __attribute__((packed));

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[];
} __attribute__((packed));

/* One configured virtqueue. Memory pointed to by `desc`/`avail`/`used`
 * is allocated by the caller (typically as static buffers in BSS),
 * sized for `size` entries each. The driver tracks `last_used_idx`
 * to detect newly-completed entries against `used->idx` (which the
 * device increments). */
typedef struct {
    uint16_t size;
    uint16_t last_used_idx;
    uint16_t free_head;     /* head of the free-descriptor list */
    uint16_t num_free;
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
} virtq_t;

/* One configured virtio-mmio device. Held by the device-specific
 * driver (virtio_input_t, virtio_blk_t, ...). */
typedef struct {
    uintptr_t base;
    uint32_t  device_id;    /* VIRTIO_ID_* */
} virtio_dev_t;

/* Probe a single MMIO slot. Returns the device id (0 if the slot is
 * empty, has the legacy version, or doesn't have a virt magic). */
uint32_t virtio_probe(uintptr_t base);

/* Walk all `virtio,mmio` nodes in the FDT, calling `cb(base, id, ctx)`
 * for every populated slot (device_id != 0). Stops as soon as `cb`
 * returns true, returning that slot's base; returns 0 if no callback
 * accepted. */
uintptr_t virtio_find_fdt(const fdt_t *fdt,
                          bool (*cb)(uintptr_t base, uint32_t id, void *ctx),
                          void *ctx);

/* Begin the modern init handshake on a probed device:
 *   RESET → ACK → DRIVER → negotiate features → FEATURES_OK → check.
 *
 * `want_lo`/`want_hi` are the 64 feature bits (low / high) the driver
 * is willing to accept. The driver's offered subset is masked against
 * the device's advertised features and written back. VERSION_1 (bit
 * 32) is added unconditionally — it's mandatory for modern.
 *
 * Returns true on success (FEATURES_OK still set after readback);
 * false if the device rejected our feature subset, in which case the
 * device is left in FAILED state and the caller should give up. */
bool virtio_init(virtio_dev_t *dev, uintptr_t base,
                 uint32_t want_lo, uint32_t want_hi);

/* Configure one queue. Caller provides:
 *   `size`   — number of entries (must be a power of two ≤ QUEUE_NUM_MAX)
 *   `desc`   — pointer to size×16 bytes, 16-byte aligned
 *   `avail`  — pointer to 6 + size×2 bytes, 2-byte aligned
 *   `used`   — pointer to 6 + size×8 bytes, 4-byte aligned
 *
 * The function zero-initialises the rings and sets the queue ready.
 * Returns false if the requested size exceeds QUEUE_NUM_MAX. */
bool virtio_queue_setup(virtio_dev_t *dev, uint32_t qsel,
                        virtq_t *q, uint16_t size,
                        void *desc, void *avail, void *used);

/* Final step of init: write DRIVER_OK. After this the device starts
 * processing queued requests. */
void virtio_finalize(virtio_dev_t *dev);

/* Push one descriptor-chain head into the avail ring. `head` is the
 * first descriptor index in a chain you've already prepared in
 * q->desc[]; if the chain has multiple descriptors they should be
 * linked via desc.next/desc.flags=NEXT. After this call, the device
 * is permitted to consume the chain. */
void virtq_avail_push(virtq_t *q, uint16_t head);

/* Pop one used-ring entry. Returns true and fills *id (head index)
 * and *len (bytes written by the device) if a new entry is present;
 * false if used.idx == last_used_idx (no progress). Non-blocking. */
bool virtq_used_pop(virtq_t *q, uint16_t *id, uint32_t *len);

/* Notify the device that we've added entries to a queue. Must be
 * called after virtq_avail_push for the device to wake up. */
void virtio_notify(virtio_dev_t *dev, uint32_t qsel);

/* Free-list helpers — the driver maintains a tiny LIFO of unused
 * descriptor indices in q->free_head/num_free, threaded through
 * desc.next. Initialise after virtio_queue_setup. */
void virtq_free_init(virtq_t *q);
uint16_t virtq_alloc_desc(virtq_t *q);    /* returns 0xFFFF if empty */
void virtq_free_desc(virtq_t *q, uint16_t idx);
