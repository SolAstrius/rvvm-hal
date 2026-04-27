#include "hid.h"
#include "i2c.h"
#include "rvvm.h"
#include "uart.h"
#include <stddef.h>

/* USB HID modifier-key usage codes (these aren't in rvvm.h yet but
 * they're the standard 0xE0-0xE7 range from the HID Usage Tables). */
#define HID_KEY_LCTRL    0xE0
#define HID_KEY_LSHIFT   0xE1
#define HID_KEY_LALT     0xE2
#define HID_KEY_LMETA    0xE3
#define HID_KEY_RCTRL    0xE4
#define HID_KEY_RSHIFT   0xE5
#define HID_KEY_RALT     0xE6
#define HID_KEY_RMETA    0xE7

void hid_kb_init(hid_keyboard_t *kb, uint8_t i2c_addr) {
    kb->i2c_addr = i2c_addr;
    for (int i = 0; i < 6; i++) kb->prev_keys[i] = 0;
    kb->prev_mod = 0;
    kb->initialised = true;
}

/* Was usage code `u` in the array? */
static bool in_array(const uint8_t *arr, int n, uint8_t u) {
    if (u == 0) return false;
    for (int i = 0; i < n; i++) if (arr[i] == u) return true;
    return false;
}

int hid_kb_poll(hid_keyboard_t *kb,
                void (*cb)(uint8_t usage, bool pressed, void *ctx),
                void *ctx) {
    if (!kb->initialised) return 0;

    /* Select INPUT_REG (0x0003 LE), then read 10 bytes. */
    uint8_t reg_sel[2] = { I2C_HID_REG_INPUT & 0xFF,
                           (I2C_HID_REG_INPUT >> 8) & 0xFF };
    uint8_t buf[RVVM_HID_KB_REPORT_LEN] = {0};

    if (!i2c_write_then_read(kb->i2c_addr, reg_sel, 2,
                             buf, sizeof(buf))) {
        return 0;        /* slave busy / no report — try next frame */
    }

    /* Length=0 means the input queue was empty when we read. */
    uint16_t len = buf[0] | ((uint16_t)buf[1] << 8);
    if (len == 0) return 0;
    if (len > sizeof(buf)) len = sizeof(buf);

    int events = 0;
    uint8_t mod  = buf[2];
    const uint8_t *keys = &buf[4];   /* 6 entries */

    /* Diff modifier byte bit-by-bit, mapping each bit to its usage code. */
    static const uint8_t mod_usages[8] = {
        HID_KEY_LCTRL, HID_KEY_LSHIFT, HID_KEY_LALT, HID_KEY_LMETA,
        HID_KEY_RCTRL, HID_KEY_RSHIFT, HID_KEY_RALT, HID_KEY_RMETA,
    };
    uint8_t mod_diff = mod ^ kb->prev_mod;
    for (int b = 0; b < 8; b++) {
        if (mod_diff & (1u << b)) {
            cb(mod_usages[b], (mod & (1u << b)) != 0, ctx);
            events++;
        }
    }

    /* Releases: keys in prev that are no longer in current. */
    for (int i = 0; i < 6; i++) {
        uint8_t u = kb->prev_keys[i];
        if (u == 0) continue;
        if (!in_array(keys, 6, u)) {
            cb(u, false, ctx);
            events++;
        }
    }
    /* Presses: keys in current that weren't in prev. */
    for (int i = 0; i < 6; i++) {
        uint8_t u = keys[i];
        if (u == 0) continue;
        if (!in_array(kb->prev_keys, 6, u)) {
            cb(u, true, ctx);
            events++;
        }
    }

    /* Snapshot for next diff. */
    kb->prev_mod = mod;
    for (int i = 0; i < 6; i++) kb->prev_keys[i] = keys[i];
    return events;
}
