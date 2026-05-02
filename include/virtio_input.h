/* virtio-input — keyboards / mice / tablets behind virtio-mmio.
 *
 * QEMU exposes these via `-device virtio-keyboard-device` (or
 * mouse/tablet/multitouch) on the `virt` machine, and the host's
 * input bytes show up as 8-byte events in the eventq:
 *
 *   struct virtio_input_event {
 *       __le16 type;     // EV_SYN=0, EV_KEY=1, EV_REL=2, EV_ABS=3
 *       __le16 code;     // Linux keycode (KEY_A=30, ...) or REL_X=0, etc
 *       __le32 value;    // EV_KEY: 0=up, 1=down, 2=repeat (we ignore)
 *   };
 *
 * We submit empty buffers into the eventq up front; the device fills
 * one per event and bumps used.idx. The statusq is for driver→device
 * (e.g. LED toggles); we don't drive it.
 *
 * This driver is the foundation under hid.c's virtio backend — the
 * outward HID API stays in USB-usage codes (HID_KEY_*), and the
 * Linux→USB translation happens here.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "virtio.h"

/* Event types (Linux input.h subset). */
#define VIRTIO_INPUT_EV_SYN          0x00
#define VIRTIO_INPUT_EV_KEY          0x01
#define VIRTIO_INPUT_EV_REL          0x02
#define VIRTIO_INPUT_EV_ABS          0x03

/* Config-space selectors (modern virtio-input §5.8.4). Used by
 * virtio_input_query_name() to identify the device subtype. */
#define VIRTIO_INPUT_CFG_ID_NAME     0x01
#define VIRTIO_INPUT_CFG_EV_BITS     0x11

/* Number of empty event buffers we keep posted at any time. The
 * device fills these and signals used; we drain in poll() and
 * re-post. 16 is plenty — even fast typing rarely produces more
 * than 4-5 events between polls. */
#define VIRTIO_INPUT_QSIZE           16

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed));

typedef struct {
    virtio_dev_t dev;
    virtq_t      eventq;
    /* Storage for the eventq's rings + the per-descriptor event
     * buffers. All zero-init in BSS. Each slot holds one 8-byte
     * event; the descriptor for slot i points at evt_buf[i]. */
    struct virtq_desc                desc[VIRTIO_INPUT_QSIZE];
    /* avail = 4 + size*2; round to alignment */
    uint8_t avail_buf[4 + VIRTIO_INPUT_QSIZE * 2 + 8]
        __attribute__((aligned(16)));
    uint8_t used_buf [4 + VIRTIO_INPUT_QSIZE * 8 + 8]
        __attribute__((aligned(16)));
    struct virtio_input_event        evt_buf[VIRTIO_INPUT_QSIZE]
        __attribute__((aligned(8)));
} virtio_input_t;

/* Discover the first virtio-input device on the bus and bring it up.
 * `vi` is the caller's storage (must outlive use; usually static or
 * BSS because of the embedded buffers).
 *
 * Returns true if a virtio-input device was found and successfully
 * initialised. Returns false on a host without virtio (RVVM) or with
 * no virtio-input attached. */
bool virtio_input_init_fdt(virtio_input_t *vi, const fdt_t *fdt);

/* Drain any pending events. For each EV_KEY transition (down/up,
 * ignoring repeats) calls cb(usb_usage, pressed, ctx) where
 * usb_usage is the USB HID Keyboard/Keypad usage code (the same
 * encoding hid.h uses elsewhere). Returns the number of events
 * delivered. Re-fills the queue with fresh empty buffers as needed.
 *
 * Non-key events (EV_SYN markers, EV_REL motion, etc) are silently
 * ignored — the keyboard backend doesn't surface them. */
int virtio_input_poll(virtio_input_t *vi,
                      void (*cb)(uint8_t usage, bool pressed, void *ctx),
                      void *ctx);
