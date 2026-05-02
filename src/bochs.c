#include "bochs.h"
#include "pci.h"
#include "rvvm.h"
#include "mmio.h"
#include "uart.h"

static inline uint16_t r16(uintptr_t base, uint32_t off) {
    return mmio_r16(base + off);
}
static inline void w16(uintptr_t base, uint32_t off, uint16_t v) {
    mmio_w16(base + off, v);
}

bool bochs_init(bochs_t *bd, uint32_t w, uint32_t h) {
    pci_func_t pf;
    if (!pci_find_device(RVVM_PCI_ID_BOCHS_DISPLAY, &pf)) {
        uart_puts("bochs: device not found — was RVVM started with -bochs_display?\n");
        return false;
    }
    pci_setup_bars(&pf);

    if (pf.bar[0] == 0 || pf.bar[2] == 0) {
        uart_printf("bochs: BAR layout wrong: bar0=%p bar2=%p\n",
                    (void *)pf.bar[0], (void *)pf.bar[2]);
        return false;
    }

    bd->vram      = (uint32_t *)pf.bar[0];
    bd->vram_base = (uint32_t *)pf.bar[0];
    bd->regs      = pf.bar[2];
    bd->width     = w;
    bd->height    = h;
    bd->stride_px = w;
    bd->back_off_rows  = 0;
    bd->double_buffered = false;

    uart_printf("bochs: %u:%u.%u  vram=%p (%u KiB)  regs=%p\n",
                (uint64_t)pf.bus, (uint64_t)pf.dev, (uint64_t)pf.func,
                (void *)pf.bar[0], pf.bar_size[0] >> 10, (void *)pf.bar[2]);

    /* Verify the version register reads as a known Bochs ID (0xB0Cx). */
    uint16_t id = r16(bd->regs, BOCHS_REG_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        uart_printf("bochs: bad ID 0x%x at regs+0x500, refusing init\n",
                    (uint64_t)id);
        return false;
    }

    /* Mode-set: disable the engine, program XRES/YRES/BPP/stride, re-enable. */
    w16(bd->regs, BOCHS_REG_ENABLE,      0);
    w16(bd->regs, BOCHS_REG_XRES,        (uint16_t)w);
    w16(bd->regs, BOCHS_REG_YRES,        (uint16_t)h);
    w16(bd->regs, BOCHS_REG_BPP,         32);
    w16(bd->regs, BOCHS_REG_VIRT_WIDTH,  (uint16_t)w);
    w16(bd->regs, BOCHS_REG_VIRT_HEIGHT, (uint16_t)h);
    w16(bd->regs, BOCHS_REG_X_OFFSET,    0);
    w16(bd->regs, BOCHS_REG_Y_OFFSET,    0);
    w16(bd->regs, BOCHS_REG_ENABLE,      BOCHS_ENABLE);    /* clears VRAM */

    return true;
}

bool bochs_enable_double_buffer(bochs_t *bd) {
    if (bd->double_buffered) return true;   /* idempotent */

    /* Two framebuffers worth of VRAM. RVVM ships 16 MiB; e.g. 800×480
     * × 4 bytes = ~1.5 MiB per frame, plenty of headroom. */
    uint64_t per_frame_bytes = (uint64_t)bd->stride_px * bd->height * 4;
    if (per_frame_bytes * 2 > RVVM_BOCHS_VRAM_SIZE) {
        uart_printf("bochs: VRAM too small for double-buffer at %ux%u "
                    "(needs %u KiB, have %u KiB)\n",
                    (uint64_t)bd->width, (uint64_t)bd->height,
                    (uint64_t)(per_frame_bytes * 2 >> 10),
                    (uint64_t)RVVM_BOCHS_VRAM_SIZE >> 10);
        return false;
    }

    /* Logical framebuffer is now 2× tall. Y_OFFSET stays 0 (so the host
     * display shows the lower half — same pixels as before, currently
     * zeroed by the BOCHS_ENABLE clear in bochs_init). The back buffer
     * is the upper half; point the draw pointer at it. */
    w16(bd->regs, BOCHS_REG_VIRT_HEIGHT, (uint16_t)(bd->height * 2));
    w16(bd->regs, BOCHS_REG_Y_OFFSET,    0);
    bd->back_off_rows   = bd->height;
    bd->vram            = bd->vram_base + (uint64_t)bd->stride_px * bd->height;
    bd->double_buffered = true;
    return true;
}

void bochs_flip(bochs_t *bd) {
    if (!bd->double_buffered) return;

    /* The just-drawn back buffer becomes the new front (its row offset
     * is what we write to Y_OFFSET); the old front becomes the new back. */
    w16(bd->regs, BOCHS_REG_Y_OFFSET, (uint16_t)bd->back_off_rows);
    bd->back_off_rows = bd->back_off_rows ? 0 : bd->height;
    bd->vram          = bd->vram_base + (uint64_t)bd->stride_px * bd->back_off_rows;
}

void bochs_fill(const bochs_t *bd, uint32_t color) {
    uint32_t n = bd->width * bd->height;
    for (uint32_t i = 0; i < n; i++) bd->vram[i] = color;
}

void bochs_rect(const bochs_t *bd, uint32_t x, uint32_t y,
                uint32_t w, uint32_t h, uint32_t color) {
    if (x >= bd->width || y >= bd->height) return;
    if (x + w > bd->width)  w = bd->width  - x;
    if (y + h > bd->height) h = bd->height - y;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *p = &bd->vram[(y + row) * bd->stride_px + x];
        for (uint32_t col = 0; col < w; col++) p[col] = color;
    }
}
