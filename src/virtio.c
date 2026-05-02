#include "virtio.h"
#include "mmio.h"

/* Convenience accessors. Only 32-bit MMIO is used by the modern
 * virtio-mmio register set; we never need r16/r8 for the regs. */
static inline uint32_t r(uintptr_t base, uint32_t off) {
    return mmio_r32(base + off);
}
static inline void w(uintptr_t base, uint32_t off, uint32_t v) {
    mmio_w32(base + off, v);
}

uint32_t virtio_probe(uintptr_t base) {
    if (r(base, VIRTIO_MMIO_MAGIC)   != VIRTIO_MAGIC_VALUE) return 0;
    if (r(base, VIRTIO_MMIO_VERSION) != 2)                  return 0;
    return r(base, VIRTIO_MMIO_DEVICE_ID);
}

uintptr_t virtio_find_fdt(const fdt_t *fdt,
                          bool (*cb)(uintptr_t, uint32_t, void *),
                          void *ctx) {
    /* Walk every "virtio,mmio" node. fdt_find_compatible only returns
     * the first hit, so iterate by re-querying after each match isn't
     * possible — we duplicate the token walk here. Cheap: there are
     * only 8 slots on QEMU virt. */
    uint32_t walk = 0;
    while (walk < fdt->struct_size) {
        uint32_t tok = fdt_read_be32(fdt->struct_blob, walk);
        if (tok == FDT_BEGIN_NODE) {
            uint32_t clen = 0;
            const char *cp = (const char *)fdt_node_prop(fdt, walk,
                                                         "compatible", &clen);
            bool is_virtio = false;
            if (cp) {
                for (uint32_t p = 0; p < clen;) {
                    /* Match on first compat string. */
                    const char *needle = "virtio,mmio";
                    bool eq = true;
                    for (uint32_t k = 0; needle[k]; k++) {
                        if (cp[p + k] != needle[k]) { eq = false; break; }
                    }
                    if (eq && cp[p + 11] == 0) { is_virtio = true; break; }
                    while (p < clen && cp[p]) p++;
                    p++;
                }
            }
            if (is_virtio) {
                uint64_t at = 0, sz = 0;
                if (fdt_node_reg64(fdt, walk, 0, &at, &sz)) {
                    uint32_t id = virtio_probe((uintptr_t)at);
                    if (id != 0 && cb((uintptr_t)at, id, ctx)) {
                        return (uintptr_t)at;
                    }
                }
            }
            /* Advance past BEGIN_NODE + name. */
            walk += 4;
            while (walk < fdt->struct_size && fdt->struct_blob[walk]) walk++;
            walk = (walk + 4) & ~3U;
        } else if (tok == FDT_END_NODE || tok == FDT_NOP) {
            walk += 4;
        } else if (tok == FDT_PROP) {
            uint32_t plen = fdt_read_be32(fdt->struct_blob, walk + 4);
            walk += 12 + ((plen + 3) & ~3U);
        } else {
            break;
        }
    }
    return 0;
}

bool virtio_init(virtio_dev_t *dev, uintptr_t base,
                 uint32_t want_lo, uint32_t want_hi) {
    dev->base = base;
    dev->device_id = r(base, VIRTIO_MMIO_DEVICE_ID);

    /* Reset (§3.1.1 step 1). */
    w(base, VIRTIO_MMIO_STATUS, 0);
    while (r(base, VIRTIO_MMIO_STATUS) != 0) { /* spin */ }

    /* ACK + DRIVER (steps 2-3). */
    w(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    w(base, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation (step 4). VERSION_1 (bit 32) is mandatory
     * for modern devices, so we OR it into the high half regardless
     * of what the caller asked for. */
    want_hi |= 1U << (VIRTIO_F_VERSION_1_BIT - 32);

    w(base, VIRTIO_MMIO_DEVICE_FEAT_SEL, 0);
    uint32_t dev_lo = r(base, VIRTIO_MMIO_DEVICE_FEAT);
    w(base, VIRTIO_MMIO_DEVICE_FEAT_SEL, 1);
    uint32_t dev_hi = r(base, VIRTIO_MMIO_DEVICE_FEAT);

    w(base, VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    w(base, VIRTIO_MMIO_DRIVER_FEAT,     dev_lo & want_lo);
    w(base, VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    w(base, VIRTIO_MMIO_DRIVER_FEAT,     dev_hi & want_hi);

    /* FEATURES_OK then re-read; if it didn't stick the device has
     * rejected our subset (step 6). */
    w(base, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
        | VIRTIO_STATUS_FEATURES_OK);
    if (!(r(base, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        w(base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        return false;
    }
    return true;
}

bool virtio_queue_setup(virtio_dev_t *dev, uint32_t qsel, virtq_t *q,
                        uint16_t size,
                        void *desc, void *avail, void *used) {
    w(dev->base, VIRTIO_MMIO_QUEUE_SEL, qsel);
    uint32_t qmax = r(dev->base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax == 0 || size > qmax) return false;

    q->size          = size;
    q->last_used_idx = 0;
    q->desc  = (struct virtq_desc  *)desc;
    q->avail = (struct virtq_avail *)avail;
    q->used  = (struct virtq_used  *)used;

    /* Zero the rings — devices may sample any field on the first
     * notify, and BSS-allocated buffers are already zero, but we're
     * also called for re-init paths. */
    for (uint16_t i = 0; i < size; i++) {
        q->desc[i].addr = 0;
        q->desc[i].len  = 0;
        q->desc[i].flags = 0;
        q->desc[i].next  = 0;
    }
    q->avail->flags = 0;
    q->avail->idx   = 0;
    q->used->flags  = 0;
    q->used->idx    = 0;

    w(dev->base, VIRTIO_MMIO_QUEUE_NUM, size);

    uintptr_t da = (uintptr_t)desc;
    uintptr_t aa = (uintptr_t)avail;
    uintptr_t ua = (uintptr_t)used;
    w(dev->base, VIRTIO_MMIO_QUEUE_DESC_LO,    (uint32_t)da);
    w(dev->base, VIRTIO_MMIO_QUEUE_DESC_HI,    (uint32_t)(da >> 32));
    w(dev->base, VIRTIO_MMIO_QUEUE_DRIVER_LO,  (uint32_t)aa);
    w(dev->base, VIRTIO_MMIO_QUEUE_DRIVER_HI,  (uint32_t)(aa >> 32));
    w(dev->base, VIRTIO_MMIO_QUEUE_DEVICE_LO,  (uint32_t)ua);
    w(dev->base, VIRTIO_MMIO_QUEUE_DEVICE_HI,  (uint32_t)(ua >> 32));

    w(dev->base, VIRTIO_MMIO_QUEUE_READY, 1);
    return true;
}

void virtio_finalize(virtio_dev_t *dev) {
    w(dev->base, VIRTIO_MMIO_STATUS,
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER
        | VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
}

void virtq_avail_push(virtq_t *q, uint16_t head) {
    /* Slot is avail.idx mod queue size. The device reads avail.idx
     * with acquire semantics, so a fence between the ring write and
     * the idx update is required to keep the device from observing a
     * stale ring entry under our advanced idx. */
    uint16_t slot = q->avail->idx % q->size;
    q->avail->ring[slot] = head;
    __asm__ volatile ("fence ow, ow" ::: "memory");
    q->avail->idx++;
}

bool virtq_used_pop(virtq_t *q, uint16_t *id, uint32_t *len) {
    /* Acquire-fence the read of used.idx so any side-effects the
     * device performed (e.g. filling our buffer) are visible before
     * we deref it. */
    __asm__ volatile ("fence ir, ir" ::: "memory");
    uint16_t cur = q->used->idx;
    if (cur == q->last_used_idx) return false;

    uint16_t slot = q->last_used_idx % q->size;
    if (id)  *id  = (uint16_t)q->used->ring[slot].id;
    if (len) *len = q->used->ring[slot].len;
    q->last_used_idx++;
    return true;
}

void virtio_notify(virtio_dev_t *dev, uint32_t qsel) {
    w(dev->base, VIRTIO_MMIO_QUEUE_NOTIFY, qsel);
}

void virtq_free_init(virtq_t *q) {
    /* Build a singly-linked free list across desc.next, head=0. */
    for (uint16_t i = 0; i < q->size - 1; i++) q->desc[i].next = i + 1;
    q->desc[q->size - 1].next = 0;   /* end marker; num_free guards us */
    q->free_head = 0;
    q->num_free  = q->size;
}

uint16_t virtq_alloc_desc(virtq_t *q) {
    if (q->num_free == 0) return 0xFFFF;
    uint16_t idx = q->free_head;
    q->free_head = q->desc[idx].next;
    q->num_free--;
    return idx;
}

void virtq_free_desc(virtq_t *q, uint16_t idx) {
    q->desc[idx].next = q->free_head;
    q->free_head = idx;
    q->num_free++;
}
