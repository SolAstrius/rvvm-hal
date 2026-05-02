/* Unified graphics HAL — auto-selects between RVVM's two framebuffer
 * surfaces:
 *
 *   1. Bochs Display (PCI 1234:1111). Available when RVVM is started
 *      with `-bochs_display`. Mode-settable: caller picks W×H, the
 *      driver mode-sets to those dimensions. 16 MiB of VRAM gives
 *      plenty of headroom (max ~2048×2048 XRGB8888).
 *
 *   2. simple-framebuffer (FDT compatible "simple-framebuffer").
 *      Available when RVVM is started with `-res WxH` instead of
 *      `-bochs_display`. Dimensions are fixed by RVVM at machine-
 *      construction time; caller's want_w / want_h are advisory and
 *      ignored.
 *
 * Bochs is preferred when available because it lets the firmware
 * choose its own resolution. simplefb is the fallback for headless or
 * resolution-locked setups.
 *
 * Both backends expose the framebuffer as a writable XRGB8888 (a8r8g8b8)
 * pixel array. simplefb advertises its format string in FDT — gfx_init
 * accepts a8r8g8b8 (matches XRGB8888 layout) and a8b8g8r8 (with a
 * runtime-flagged channel-swap so callers know to adapt). Other formats
 * (r5g6b5, r8g8b8, b8g8r8) are rejected as incompatible with the
 * standard 32-bit pipeline. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fdt.h"

typedef enum {
    GFX_BACKEND_NONE       = 0,
    GFX_BACKEND_BOCHS      = 1,
    GFX_BACKEND_SIMPLEFB   = 2,
    GFX_BACKEND_VIRTIO_GPU = 3,
} gfx_backend_t;

typedef enum {
    GFX_FMT_XRGB8888 = 0,   /* a8r8g8b8: native, what bochs always uses */
    GFX_FMT_XBGR8888 = 1,   /* a8b8g8r8: red/blue swapped vs XRGB */
} gfx_format_t;

typedef struct {
    uint32_t      *vram;       /* writable framebuffer, row-major */
    uint32_t       width;      /* visible pixels per row */
    uint32_t       height;     /* visible rows */
    uint32_t       stride_px;  /* pixels per row in memory (>= width) */
    gfx_backend_t  backend;
    gfx_format_t   format;
} gfx_t;

/* Probe and bring up whichever backend is available, in priority order
 * (Bochs > simplefb). For Bochs the framebuffer is mode-set to
 * want_w×want_h; for simplefb the size is whatever RVVM was started
 * with. Returns false if neither backend is present.
 *
 * After init: writes to g->vram[y * g->stride_px + x] hit the screen
 * directly. Pixels are 32-bit; the caller is responsible for honouring
 * g->format if they care about exact channel order. The gfx_pixel /
 * gfx_fill / gfx_rect helpers below take XRGB8888 input and convert
 * automatically. */
bool gfx_init(gfx_t *g, uint32_t want_w, uint32_t want_h);

/* Same as gfx_init but additionally tries simple-framebuffer when Bochs
 * isn't present. Pass the same `fdt_t` you initialised with fdt_init().
 * This is the form most consumers want — chip-8 / zx-spectrum / etc. all
 * have an FDT handle already by the time they bring up graphics. */
bool gfx_init_fdt(gfx_t *g, const fdt_t *fdt, uint32_t want_w, uint32_t want_h);

/* Fill the entire visible region with one XRGB8888 colour. */
void gfx_fill(const gfx_t *g, uint32_t color);

/* Plot a single pixel — no bounds check. Auto-swizzles if backend is
 * XBGR8888. The XRGB8888 case compiles down to one store. */
static inline void gfx_pixel(const gfx_t *g, uint32_t x, uint32_t y, uint32_t color) {
    if (g->format == GFX_FMT_XBGR8888) {
        uint32_t r = (color >> 16) & 0xFF;
        uint32_t b = (color >>  0) & 0xFF;
        color = (color & 0xFF00FF00U) | (b << 16) | r;
    }
    g->vram[y * g->stride_px + x] = color;
}

/* Fill an axis-aligned rectangle. Bounds-clamped against (width, height). */
void gfx_rect(const gfx_t *g, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h, uint32_t color);

/* Present a sub-rectangle to the host display.
 *
 * On Bochs and simple-framebuffer backends this is a no-op — those
 * are direct-mapped, so writes to vram are already visible.
 *
 * On virtio-gpu it issues TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for
 * the rect. gfx_rect / gfx_fill auto-call this internally, so most
 * callers don't need to touch it; consumers that write through the
 * raw vram pointer themselves (chip-8, zx-spectrum) should call
 * gfx_present_all(g) once per frame to push their changes. */
void gfx_present(const gfx_t *g, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h);
static inline void gfx_present_all(const gfx_t *g) {
    gfx_present(g, 0, 0, g->width, g->height);
}

/* Switch into page-flipped double-buffer mode. After a successful
 * return:
 *   - g->vram points at an off-screen back buffer; consumers draw
 *     there as before.
 *   - gfx_flip(g) atomically swaps which buffer is on-screen. The
 *     host display only ever sees whole frames, never a half-written
 *     mix — fixes the tearing class of artifacts that show up when
 *     a slow per-frame draw races the host display's scanout.
 *   - g->vram is updated by gfx_flip() to point at the new back
 *     buffer; consumers that cache the pointer must re-read it after
 *     each flip.
 * Returns false if the active backend has no native page-flip support
 * (today: simple-framebuffer; virtio-gpu's transfer+flush is already
 * tear-free, so gfx_enable_double_buffer trivially succeeds without
 * changing the surface).
 *
 * Idempotent — safe to call twice. */
bool gfx_enable_double_buffer(gfx_t *g);

/* Atomically present the just-drawn back buffer. After the call,
 * g->vram points at the new back; that whole-frame swap is what makes
 * the output tear-free.
 *
 * If double-buffering wasn't enabled this falls back to gfx_present_all
 * (no-op for direct-mapped backends, transfer+flush for virtio-gpu)
 * so it's safe to use from a single render path that may or may not
 * have double-buffering available. */
void gfx_flip(gfx_t *g);
