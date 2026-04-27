/* HID-over-I²C keyboard client. Polls the input register and emits
 * up/down events by diffing the current report against the previous.
 *
 * RVVM emits 10-byte boot-keyboard reports (no report ID byte for the
 * keyboard widget):
 *   [0..1]  total report length, little-endian = 10
 *   [2]     modifier byte (LCtrl/LShift/LAlt/LMeta/RCtrl/...)
 *   [3]     reserved (0)
 *   [4..9]  up to 6 USB-HID usage codes for currently-pressed keys
 *
 * USB usage codes for the relevant ASCII keys are in src/devices/hid_api.h
 * in RVVM (e.g. HID_KEY_A = 0x04, HID_KEY_1 = 0x1e). */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t i2c_addr;        /* RVVM keyboard sits at 0x08 */
    uint8_t prev_keys[6];    /* last seen pressed-key array */
    uint8_t prev_mod;        /* last seen modifier byte */
    bool    initialised;
} hid_keyboard_t;

void hid_kb_init(hid_keyboard_t *kb, uint8_t i2c_addr);

/* Drain the keyboard's input report. For each key whose pressed/released
 * state changed since the last poll, calls cb(usage, pressed, ctx).
 * Returns the number of events emitted (0 means nothing changed).
 *
 * Modifier keys are reported with their corresponding USB usage codes
 * (HID_KEY_LEFTCTRL = 0xE0 .. HID_KEY_RIGHTMETA = 0xE7) so callers
 * can treat them uniformly. */
int hid_kb_poll(hid_keyboard_t *kb,
                void (*cb)(uint8_t usage, bool pressed, void *ctx),
                void *ctx);
