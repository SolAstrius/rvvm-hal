/* Bochs Display driver (RVVM PCI device 0x1234:0x1111).
 * Mode-sets the framebuffer to 640×400 XRGB8888 and exposes the
 * VRAM pointer for direct pixel writes. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct bochs_s {
    uint32_t *vram;            /* BAR0, points at the linear framebuffer */
    uintptr_t regs;            /* BAR2, where the BOCHS_REG_* live */
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride_px;       /* pixels per scanline (== width unless panning) */
} bochs_t;

/* Probe via PCI, allocate BARs, mode-set to (w × h × 32 bpp).
 * Returns false if the device isn't present. */
bool bochs_init(bochs_t *bd, uint32_t w, uint32_t h);

/* Convenience: fill the whole framebuffer with one XRGB8888 colour. */
void bochs_fill(const bochs_t *bd, uint32_t color);

/* Plot a single pixel; no bounds check. */
static inline void bochs_pixel(const bochs_t *bd, uint32_t x, uint32_t y, uint32_t c) {
    bd->vram[y * bd->stride_px + x] = c;
}

/* Fill an axis-aligned rectangle. */
void bochs_rect(const bochs_t *bd, uint32_t x, uint32_t y,
                uint32_t w, uint32_t h, uint32_t color);
