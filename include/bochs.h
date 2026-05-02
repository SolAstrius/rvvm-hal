/* Bochs Display driver (RVVM PCI device 0x1234:0x1111).
 * Mode-sets the framebuffer to 640×400 XRGB8888 and exposes the
 * VRAM pointer for direct pixel writes. */

#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct bochs_s {
    uint32_t *vram;            /* current draw target — back buffer when DB,
                                  same as vram_base otherwise. */
    uint32_t *vram_base;       /* BAR0 — physical start of VRAM, immutable. */
    uintptr_t regs;            /* BAR2, where the BOCHS_REG_* live */
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride_px;       /* pixels per scanline (== width unless panning) */
    /* Double-buffer state (only valid when double_buffered). The visible
     * region starts at scanline (back_off_rows == 0 ? height : 0); the
     * back buffer is at the *other* half. After bochs_flip() these swap. */
    uint32_t  back_off_rows;
    bool      double_buffered;
} bochs_t;

/* Probe via PCI, allocate BARs, mode-set to (w × h × 32 bpp).
 * Returns false if the device isn't present. */
bool bochs_init(bochs_t *bd, uint32_t w, uint32_t h);

/* Switch into page-flipped double-buffer mode. Must be called after
 * bochs_init(); a successful return means VIRT_HEIGHT has been doubled
 * and `bd->vram` now points at the off-screen back half. The first
 * bochs_flip() makes whatever the caller drew there visible.
 *
 * Returns false if VRAM isn't large enough for two frames at the
 * current mode (shouldn't happen — RVVM ships 16 MiB). */
bool bochs_enable_double_buffer(bochs_t *bd);

/* Atomically swap front/back. After this returns, `bd->vram` points at
 * the new back buffer; what was just drawn is now visible. One 16-bit
 * write to BOCHS_REG_Y_OFFSET — the host display picks up the new
 * scanout origin on its next refresh, so consumers see whole frames or
 * the previous whole frame, never a half-written mix.
 *
 * No-op if double-buffering wasn't enabled. */
void bochs_flip(bochs_t *bd);

/* Convenience: fill the whole framebuffer with one XRGB8888 colour. */
void bochs_fill(const bochs_t *bd, uint32_t color);

/* Plot a single pixel; no bounds check. */
static inline void bochs_pixel(const bochs_t *bd, uint32_t x, uint32_t y, uint32_t c) {
    bd->vram[y * bd->stride_px + x] = c;
}

/* Fill an axis-aligned rectangle. */
void bochs_rect(const bochs_t *bd, uint32_t x, uint32_t y,
                uint32_t w, uint32_t h, uint32_t color);
