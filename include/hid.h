/* HID keyboard — host-agnostic API with two interchangeable backends.
 *
 * Backends (auto-selected from FDT):
 *   - I²C-HID   : RVVM's `opencores,i2c-ocores` + i2c-hid keyboard
 *   - virtio    : QEMU's `virtio-keyboard-device` over virtio-mmio
 *
 * Outward shape is identical: callers see a `hid_keyboard_t`, init
 * it once, then call `hid_kb_poll()` from their main loop. The poll
 * callback receives USB HID Keyboard/Keypad usage codes regardless
 * of backend — virtio-input's Linux keycodes are translated to the
 * same encoding the I²C path emits, so consumer code (the on_key
 * handler in `examples/probe/`) doesn't care which host it's on.
 *
 * Two init entry points:
 *   - hid_kb_init(kb, i2c_addr) — legacy. Forces the I²C backend at
 *     the given address. Kept for callers that already discovered
 *     the bus themselves (RVVM-only firmware).
 *   - hid_kb_init_fdt(kb, fdt)  — preferred. Walks the FDT, picks
 *     the right backend based on what's there, and falls back to
 *     I²C-HID on RVVM. Returns false if neither host has a
 *     keyboard wired up.
 *
 * Both leave `kb` ready for `hid_kb_poll()`; mixing init styles per
 * struct is unsupported.
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"
#include "virtio_input.h"

typedef enum {
    HID_BACKEND_NONE   = 0,
    HID_BACKEND_I2C    = 1,
    HID_BACKEND_VIRTIO = 2,
} hid_backend_t;

typedef struct {
    hid_backend_t backend;
    bool          initialised;

    /* I²C-HID state (used when backend == I2C). */
    uint8_t i2c_addr;
    uint8_t prev_keys[6];
    uint8_t prev_mod;

    /* virtio-input state (used when backend == VIRTIO). The struct
     * embeds its own queue + buffers; we keep a pointer to the
     * caller-owned virtio_input_t so multiple hid_keyboard_t's can
     * (in principle) share one device, but for the common case we
     * use a single static virtio_input_t inside hid.c. */
    virtio_input_t *vi;
} hid_keyboard_t;

/* Legacy init: forces the I²C-HID backend at `i2c_addr`. After this
 * call, hid_kb_poll(kb, ...) reads from the i2c-hid device. */
void hid_kb_init(hid_keyboard_t *kb, uint8_t i2c_addr);

/* Auto-discovery init: walk the FDT and bring up whichever keyboard
 * backend the host advertises.
 *   On RVVM: finds opencores-i2c at its FDT node, uses I²C-HID at
 *            address RVVM_I2C_HID_KEYBOARD.
 *   On QEMU: finds the first virtio-input device on virtio-mmio.
 * Returns true if a keyboard was found and initialised. */
bool hid_kb_init_fdt(hid_keyboard_t *kb, const fdt_t *fdt);

/* Drain pending keyboard activity, calling cb(usage, pressed, ctx)
 * once per key state transition since the previous poll. Identical
 * semantics across backends. */
int hid_kb_poll(hid_keyboard_t *kb,
                void (*cb)(uint8_t usage, bool pressed, void *ctx),
                void *ctx);
