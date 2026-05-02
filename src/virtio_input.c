#include "virtio_input.h"
#include "uart.h"
#include <stddef.h>

/* Linux keycode → USB HID usage. Indexed by Linux keycode (8-bit
 * subset; the keycodes >127 are mostly multimedia / system and not
 * needed for HID-style boot keyboard emulation). 0 means "no usage"
 * — the dispatcher just drops those events. */
static const uint8_t lkc_to_usb[128] = {
    [1]   = 0x29,   /* ESC */
    [2]   = 0x1E, [3]   = 0x1F, [4]   = 0x20, [5]   = 0x21, [6]   = 0x22,
    [7]   = 0x23, [8]   = 0x24, [9]   = 0x25, [10]  = 0x26, [11]  = 0x27,
    [12]  = 0x2D,   /* - */
    [13]  = 0x2E,   /* = */
    [14]  = 0x2A,   /* BACKSPACE */
    [15]  = 0x2B,   /* TAB */
    [16]  = 0x14, [17]  = 0x1A, [18]  = 0x08, [19]  = 0x15, [20]  = 0x17,
    [21]  = 0x1C, [22]  = 0x18, [23]  = 0x0C, [24]  = 0x12, [25]  = 0x13,
    [26]  = 0x2F,   /* [ */
    [27]  = 0x30,   /* ] */
    [28]  = 0x28,   /* ENTER */
    [29]  = 0xE0,   /* LCTRL */
    [30]  = 0x04, [31]  = 0x16, [32]  = 0x07, [33]  = 0x09, [34]  = 0x0A,
    [35]  = 0x0B, [36]  = 0x0D, [37]  = 0x0E, [38]  = 0x0F,
    [39]  = 0x33,   /* ; */
    [40]  = 0x34,   /* ' */
    [41]  = 0x35,   /* ` */
    [42]  = 0xE1,   /* LSHIFT */
    [43]  = 0x31,   /* \ */
    [44]  = 0x1D, [45]  = 0x1B, [46]  = 0x06, [47]  = 0x19, [48]  = 0x05,
    [49]  = 0x11, [50]  = 0x10,
    [51]  = 0x36,   /* , */
    [52]  = 0x37,   /* . */
    [53]  = 0x38,   /* / */
    [54]  = 0xE5,   /* RSHIFT */
    [55]  = 0x55,   /* KPASTERISK */
    [56]  = 0xE2,   /* LALT */
    [57]  = 0x2C,   /* SPACE */
    [58]  = 0x39,   /* CAPSLOCK */
    [59]  = 0x3A, [60]  = 0x3B, [61]  = 0x3C, [62]  = 0x3D, [63]  = 0x3E,
    [64]  = 0x3F, [65]  = 0x40, [66]  = 0x41, [67]  = 0x42, [68]  = 0x43,
    [69]  = 0x53,   /* NUMLOCK */
    [70]  = 0x47,   /* SCROLLLOCK */
    [71]  = 0x5F, [72]  = 0x60, [73]  = 0x61,
    [74]  = 0x56,   /* KPMINUS */
    [75]  = 0x5C, [76]  = 0x5D, [77]  = 0x5E,
    [78]  = 0x57,   /* KPPLUS */
    [79]  = 0x59, [80]  = 0x5A, [81]  = 0x5B, [82]  = 0x62,
    [83]  = 0x63,   /* KPDOT */
    [87]  = 0x44, [88]  = 0x45,
    [96]  = 0x58,   /* KPENTER */
    [97]  = 0xE4,   /* RCTRL */
    [98]  = 0x54,   /* KPSLASH */
    [99]  = 0x46,   /* PRINTSCREEN/SYSRQ */
    [100] = 0xE6,   /* RALT */
    [102] = 0x4A,   /* HOME */
    [103] = 0x52,   /* UP */
    [104] = 0x4B,   /* PAGEUP */
    [105] = 0x50,   /* LEFT */
    [106] = 0x4F,   /* RIGHT */
    [107] = 0x4D,   /* END */
    [108] = 0x51,   /* DOWN */
    [109] = 0x4E,   /* PAGEDOWN */
    [110] = 0x49,   /* INSERT */
    [111] = 0x4C,   /* DELETE */
    [125] = 0xE3,   /* LMETA */
    [126] = 0xE7,   /* RMETA */
};

/* Probe callback: take the first virtio-input device we see. */
static bool first_input(uintptr_t base, uint32_t id, void *ctx) {
    (void)base; (void)ctx;
    return id == VIRTIO_ID_INPUT;
}

/* Refill the eventq with empty descriptors pointing at evt_buf[i].
 * Called once at init and after each used-ring drain. */
static void refill_eventq(virtio_input_t *vi) {
    while (vi->eventq.num_free) {
        uint16_t idx = virtq_alloc_desc(&vi->eventq);
        if (idx == 0xFFFF) break;
        vi->eventq.desc[idx].addr  = (uint64_t)(uintptr_t)&vi->evt_buf[idx];
        vi->eventq.desc[idx].len   = sizeof(struct virtio_input_event);
        vi->eventq.desc[idx].flags = VIRTQ_DESC_F_WRITE;
        vi->eventq.desc[idx].next  = 0;
        virtq_avail_push(&vi->eventq, idx);
    }
}

bool virtio_input_init_fdt(virtio_input_t *vi, const fdt_t *fdt) {
    uintptr_t base = virtio_find_fdt(fdt, first_input, NULL);
    if (!base) return false;

    /* No special features needed — VERSION_1 is added unconditionally
     * by virtio_init. virtio-input doesn't define driver-meaningful
     * feature bits in the spec; it's all push events. */
    if (!virtio_init(&vi->dev, base, 0, 0)) return false;

    if (!virtio_queue_setup(&vi->dev, /*qsel=*/0, &vi->eventq,
                            VIRTIO_INPUT_QSIZE,
                            vi->desc, vi->avail_buf, vi->used_buf)) {
        return false;
    }
    virtq_free_init(&vi->eventq);

    /* Pre-post empty receive buffers and arm the device. */
    refill_eventq(vi);
    virtio_finalize(&vi->dev);
    virtio_notify(&vi->dev, /*qsel=*/0);
    return true;
}

int virtio_input_poll(virtio_input_t *vi,
                      void (*cb)(uint8_t usage, bool pressed, void *ctx),
                      void *ctx) {
    int events = 0;
    uint16_t id;
    uint32_t len;
    bool any = false;

    while (virtq_used_pop(&vi->eventq, &id, &len)) {
        any = true;
        if (id < VIRTIO_INPUT_QSIZE && len >= sizeof(struct virtio_input_event)) {
            const struct virtio_input_event *e = &vi->evt_buf[id];
            if (e->type == VIRTIO_INPUT_EV_KEY && e->value != 2) {
                /* value 0=up, 1=down, 2=repeat (skip). */
                bool pressed = (e->value == 1);
                uint16_t code = e->code;
                if (code < 128) {
                    uint8_t usage = lkc_to_usb[code];
                    if (usage) {
                        cb(usage, pressed, ctx);
                        events++;
                    }
                }
            }
            /* EV_SYN / EV_REL / EV_ABS / unmapped EV_KEY codes are
             * dropped silently — keyboard surface only. */
        }
        virtq_free_desc(&vi->eventq, id);
    }

    if (any) {
        refill_eventq(vi);
        virtio_notify(&vi->dev, /*qsel=*/0);
    }
    return events;
}
