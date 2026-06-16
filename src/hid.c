#include "hid.h"
#include "i2c.h"
#include "irq.h"
#include "fdt.h"
#include "rvvm.h"
#include "uart.h"
#include <stddef.h>

/* USB HID modifier-key usage codes (HID Usage Tables 0xE0-0xE7). */
#define HID_KEY_LCTRL    0xE0
#define HID_KEY_LSHIFT   0xE1
#define HID_KEY_LALT     0xE2
#define HID_KEY_LMETA    0xE3
#define HID_KEY_RCTRL    0xE4
#define HID_KEY_RSHIFT   0xE5
#define HID_KEY_RALT     0xE6
#define HID_KEY_RMETA    0xE7

/* The shared virtio-input backing for the auto-discovery path. We
 * keep one in BSS so consumers don't have to plumb a virtio_input_t
 * through their state. Multiple hid_keyboard_t's pointing at this
 * one device are fine — virtio_input_poll() drains the same eventq. */
static virtio_input_t hid_vi_singleton;

void hid_kb_init(hid_keyboard_t *kb, uint8_t i2c_addr) {
    kb->backend     = HID_BACKEND_I2C;
    kb->initialised = true;
    kb->i2c_addr    = i2c_addr;
    kb->prev_mod    = 0;
    for (int i = 0; i < 6; i++) kb->prev_keys[i] = 0;
    kb->vi          = NULL;
}

bool hid_kb_init_fdt(hid_keyboard_t *kb, const fdt_t *fdt) {
    /* Try virtio-input first — on QEMU virt with -device
     * virtio-keyboard-device this binds. On RVVM there are no
     * virtio-mmio nodes so virtio_input_init_fdt returns false fast,
     * and we fall through to the I²C path. */
    if (virtio_input_init_fdt(&hid_vi_singleton, fdt)) {
        kb->backend     = HID_BACKEND_VIRTIO;
        kb->initialised = true;
        kb->vi          = &hid_vi_singleton;
        kb->i2c_addr    = 0;
        kb->prev_mod    = 0;
        for (int i = 0; i < 6; i++) kb->prev_keys[i] = 0;
        return true;
    }

    /* I²C-HID fallback. Locate the opencores-i2c controller; if it's
     * present we assume RVVM's standard layout (kb at 0x08). */
    uint32_t i_off = fdt_find_compatible(fdt, "opencores,i2c-ocores");
    if (i_off != UINT32_MAX) {
        uint64_t at = 0, sz = 0;
        if (fdt_node_reg64(fdt, i_off, 0, &at, &sz)) {
            i2c_init((uintptr_t)at);
        }
        hid_kb_init(kb, RVVM_I2C_HID_KEYBOARD);
        return true;
    }

    /* No backend available — leave kb in HID_BACKEND_NONE; poll() is
     * a no-op so callers can still use the same struct safely. */
    kb->backend     = HID_BACKEND_NONE;
    kb->initialised = false;
    return false;
}

/* ---- I²C-HID backend (was the entire file before). ---- */

static bool in_array(const uint8_t *arr, int n, uint8_t u) {
    if (u == 0) return false;
    for (int i = 0; i < n; i++) if (arr[i] == u) return true;
    return false;
}

static int hid_poll_i2c(hid_keyboard_t *kb,
                        void (*cb)(uint8_t, bool, void *), void *ctx) {
    uint8_t reg_sel[2] = { I2C_HID_REG_INPUT & 0xFF,
                           (I2C_HID_REG_INPUT >> 8) & 0xFF };
    uint8_t buf[RVVM_HID_KB_REPORT_LEN] = {0};

    if (!i2c_write_then_read(kb->i2c_addr, reg_sel, 2, buf, sizeof(buf))) {
        static bool warned;
        if (!warned) {
            uart_printf("hid: i2c xact to addr 0x%x failed\n",
                        (uint64_t)kb->i2c_addr);
            warned = true;
        }
        return 0;
    }

    uint16_t len = buf[0] | ((uint16_t)buf[1] << 8);
    if (len == 0) return 0;
    if (len > sizeof(buf)) len = sizeof(buf);

    int events = 0;
    uint8_t mod  = buf[2];
    const uint8_t *keys = &buf[4];

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
    for (int i = 0; i < 6; i++) {
        uint8_t u = kb->prev_keys[i];
        if (u && !in_array(keys, 6, u)) { cb(u, false, ctx); events++; }
    }
    for (int i = 0; i < 6; i++) {
        uint8_t u = keys[i];
        if (u && !in_array(kb->prev_keys, 6, u)) { cb(u, true, ctx); events++; }
    }
    kb->prev_mod = mod;
    for (int i = 0; i < 6; i++) kb->prev_keys[i] = keys[i];
    return events;
}

int hid_kb_poll(hid_keyboard_t *kb,
                void (*cb)(uint8_t usage, bool pressed, void *ctx),
                void *ctx) {
    if (!kb->initialised) return 0;
    switch (kb->backend) {
    case HID_BACKEND_I2C:
        return hid_poll_i2c(kb, cb, ctx);
    case HID_BACKEND_VIRTIO:
        return virtio_input_poll(kb->vi, cb, ctx);
    default:
        return 0;
    }
}

/* ---- Interrupt-driven path (i2c-HID only) -------------------------- */

/* RVVM exposes a single i2c-HID keyboard that raises its own PLIC line
 * (separate from the i2c controller's) whenever an input report is
 * ready. We bind one keyboard's forwarding callback here so the PLIC
 * trampoline can reach it without the dispatcher carrying per-source
 * context. */
static struct {
    hid_keyboard_t *kb;
    void (*cb)(uint8_t, bool, void *);
    void           *ctx;
} hid_irq_bind;

/* PLIC handler: drain the input report. The i2c read inside
 * hid_kb_poll() is what lowers the device's interrupt line; the
 * dispatcher's claim loop re-invokes us until the report queue empties,
 * so one poll per claim is enough even when several reports are queued. */
static void hid_irq_trampoline(uint32_t source, void *ctx) {
    (void)source;
    (void)ctx;
    if (hid_irq_bind.kb) {
        hid_kb_poll(hid_irq_bind.kb, hid_irq_bind.cb, hid_irq_bind.ctx);
    }
}

uint32_t hid_kb_irq_attach(hid_keyboard_t *kb, const fdt_t *fdt,
                           void (*cb)(uint8_t usage, bool pressed, void *ctx),
                           void *ctx) {
    /* Only the i2c-HID backend has a PLIC line. virtio-input carries no
     * `interrupts` node here; drain it from its own queue via
     * hid_kb_poll() on an idle/timer tick instead. */
    if (!kb || !kb->initialised || kb->backend != HID_BACKEND_I2C) return 0;

    /* Discover the keyboard's source from the device tree — never a
     * hardcoded PLIC number. RVVM names the i2c-HID node
     * `compatible = "hid-over-i2c"` and fills its `interrupts` cell with
     * the source it allocated. */
    uint32_t off = fdt_find_compatible(fdt, "hid-over-i2c");
    if (off == UINT32_MAX) return 0;
    uint32_t source = fdt_node_interrupt(fdt, off);
    if (!source) return 0;

    hid_irq_bind.kb  = kb;
    hid_irq_bind.cb  = cb;
    hid_irq_bind.ctx = ctx;

    irq_register(source, hid_irq_trampoline, NULL);
    irq_set_priority(source, 2);
    irq_enable(source);
    return source;
}
