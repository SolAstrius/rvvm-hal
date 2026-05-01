#include "gfx.h"
#include "bochs.h"
#include "fdt.h"
#include "uart.h"
#include "rvvm.h"
#include <stddef.h>

/* memset / memcpy live in string_asm.S (or string.c if HAL_NO_ASM_STRING).
 * Either way they're freestanding-safe; we don't pull <string.h>. */
extern void *memset(void *dst, int c, size_t n);

/* str equal helper — string.h's strcmp isn't pulled into freestanding. */
static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static bool gfx_simplefb_from_fdt(gfx_t *g, const fdt_t *fdt) {
    uint32_t off = fdt_find_compatible(fdt, "simple-framebuffer");
    if (off == UINT32_MAX) return false;

    uint64_t addr = 0, size = 0;
    if (!fdt_node_reg64(fdt, off, 0, &addr, &size)) return false;

    uint32_t w_len = 0, h_len = 0, s_len = 0, f_len = 0;
    const void *w_p = fdt_node_prop(fdt, off, "width",  &w_len);
    const void *h_p = fdt_node_prop(fdt, off, "height", &h_len);
    const void *s_p = fdt_node_prop(fdt, off, "stride", &s_len);
    const char *fmt = fdt_node_prop(fdt, off, "format", &f_len);

    if (!w_p || !h_p || !s_p || !fmt) {
        uart_puts("gfx: simple-framebuffer node missing required props\n");
        return false;
    }

    uint32_t w      = fdt_read_be32(w_p, 0);
    uint32_t h      = fdt_read_be32(h_p, 0);
    uint32_t stride = fdt_read_be32(s_p, 0);

    /* simplefb stride is in bytes; we expose pixels. Reject sub-32-bit
     * surfaces — those don't have a 1:1 pixel mapping in our 32-bit
     * pipeline and supporting them isn't useful for any consumer we
     * care about. */
    if (stride < w * 4 || (stride & 3) != 0) {
        uart_printf("gfx: simplefb stride=%u not 32-bit-pixel-aligned (w=%u)\n",
                    (uint64_t)stride, (uint64_t)w);
        return false;
    }

    gfx_format_t format;
    if      (str_eq(fmt, "a8r8g8b8")) format = GFX_FMT_XRGB8888;
    else if (str_eq(fmt, "a8b8g8r8")) format = GFX_FMT_XBGR8888;
    else {
        uart_printf("gfx: simplefb format='%s' not 32-bit RGB; refusing\n", fmt);
        return false;
    }

    g->vram      = (uint32_t *)(uintptr_t)addr;
    g->width     = w;
    g->height    = h;
    g->stride_px = stride / 4;
    g->backend   = GFX_BACKEND_SIMPLEFB;
    g->format    = format;

    uart_printf("gfx: simple-framebuffer @ %p  %ux%u  stride=%u px  fmt=%s\n",
                (void *)g->vram, (uint64_t)w, (uint64_t)h,
                (uint64_t)g->stride_px, fmt);
    return true;
}

/* Bochs path. Returns true on success; on failure quietly leaves `g`
 * untouched so the caller can try the next backend. We don't print the
 * "device not found" line here — bochs_init already does, but we don't
 * want noise in normal -res operation. So we pre-check via PCI.h. */
#include "pci.h"
static bool gfx_bochs_init(gfx_t *g, uint32_t w, uint32_t h) {
    pci_func_t pf;
    if (!pci_find_device(RVVM_PCI_ID_BOCHS_DISPLAY, &pf)) return false;

    /* Bochs is present — defer to the existing driver. If mode-set fails
     * past this point, that's a real error and bochs_init will log. */
    static bochs_t bd;
    if (!bochs_init(&bd, w, h)) return false;

    g->vram      = bd.vram;
    g->width     = bd.width;
    g->height    = bd.height;
    g->stride_px = bd.stride_px;
    g->backend   = GFX_BACKEND_BOCHS;
    g->format    = GFX_FMT_XRGB8888;
    return true;
}

/* External: caller hands us the FDT they've already initialised. We
 * could avoid the dependency by re-finding the FDT at the well-known
 * a1 boot register, but threading it through is cleaner. We keep the
 * old name `gfx_init` for the common case where the firmware doesn't
 * have an FDT handle; that variant only attempts Bochs. */
bool gfx_init(gfx_t *g, uint32_t want_w, uint32_t want_h) {
    g->backend = GFX_BACKEND_NONE;
    if (gfx_bochs_init(g, want_w, want_h)) return true;
    /* Without an FDT we can't reach simple-framebuffer; tell the caller. */
    uart_puts("gfx: no Bochs Display; pass FDT to gfx_init_fdt() to try simplefb\n");
    return false;
}

bool gfx_init_fdt(gfx_t *g, const fdt_t *fdt, uint32_t want_w, uint32_t want_h) {
    g->backend = GFX_BACKEND_NONE;
    if (gfx_bochs_init(g, want_w, want_h)) return true;
    if (gfx_simplefb_from_fdt(g, fdt))     return true;
    uart_puts("gfx: no graphics surface found (try -bochs_display or -res WxH)\n");
    return false;
}

/* Single-row fill — the workhorse under gfx_fill / gfx_rect.
 *
 * Three paths, in decreasing order of preference:
 *
 *   1. Uniform-byte pixels (b0 == b1 == b2 == b3). Common for clears
 *      (0x00 black, 0xFFFFFFFF white, 0x80808080 gray, …). Defers to
 *      memset, which is the asm-unrolled string_asm.S version when
 *      HAL_ASM_STRING is on — ~4 GiB/s vs the per-pixel sw loop.
 *
 *   2. Pair-pack: two 32-bit pixels into a 64-bit register, one `sd`
 *      per 2 px. Compiler tracks alignment; we pre-step a single
 *      32-bit pixel if the row pointer isn't 8-byte aligned (happens
 *      when gfx_rect's `x` is odd). 2× over the per-pixel sw loop.
 *
 *   3. Tail / unaligned-prefix: scalar sw for the up-to-1 leading or
 *      trailing pixel that the pair loop can't cover.
 *
 * Marked static-inline to let the compiler specialise per call site
 * (gfx_fill knows w == g->width; gfx_rect doesn't). */
static inline void gfx_fill_row(uint32_t *p, uint32_t w, uint32_t pix) {
    /* Path 1: uniform-byte → memset. Detect by xor-folding. */
    uint8_t b0 = (uint8_t)pix;
    if (((pix >>  8) & 0xFF) == b0 &&
        ((pix >> 16) & 0xFF) == b0 &&
        ((pix >> 24) & 0xFF) == b0) {
        memset(p, b0, (size_t)w * 4);
        return;
    }

    /* Path 2: pair-pack. Pre-align to 8 bytes if needed (odd x). */
    if (((uintptr_t)p & 7) && w) {
        *p++ = pix;
        w--;
    }
    uint64_t pix64 = ((uint64_t)pix << 32) | pix;
    uint64_t *p64  = (uint64_t *)p;
    uint32_t pairs = w >> 1;
    for (uint32_t i = 0; i < pairs; i++) p64[i] = pix64;
    if (w & 1) ((uint32_t *)(p64 + pairs))[0] = pix;
}

void gfx_fill(const gfx_t *g, uint32_t color) {
    if (g->backend == GFX_BACKEND_NONE) return;
    uint32_t pix = color;
    if (g->format == GFX_FMT_XBGR8888) {
        uint32_t r = (color >> 16) & 0xFF;
        uint32_t b = (color >>  0) & 0xFF;
        pix = (color & 0xFF00FF00U) | (b << 16) | r;
    }

    /* Contiguous fast path: when stride matches width, the framebuffer
     * is one flat run — fill in a single call instead of per-row. The
     * uniform-byte sub-case collapses to a single memset across the
     * whole surface. */
    if (g->stride_px == g->width) {
        gfx_fill_row(g->vram, g->width * g->height, pix);
        return;
    }

    /* Per-row path (simplefb with stride > width padding). Pointer
     * strength-reduced — increment by stride_px each iter instead of
     * recomputing y * stride_px from scratch. */
    uint32_t *p = g->vram;
    for (uint32_t y = 0; y < g->height; y++) {
        gfx_fill_row(p, g->width, pix);
        p += g->stride_px;
    }
}

void gfx_rect(const gfx_t *g, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h, uint32_t color) {
    if (g->backend == GFX_BACKEND_NONE) return;
    if (x >= g->width || y >= g->height) return;
    if (x + w > g->width)  w = g->width  - x;
    if (y + h > g->height) h = g->height - y;

    uint32_t pix = color;
    if (g->format == GFX_FMT_XBGR8888) {
        uint32_t r = (color >> 16) & 0xFF;
        uint32_t b = (color >>  0) & 0xFF;
        pix = (color & 0xFF00FF00U) | (b << 16) | r;
    }

    /* Pointer strength-reduced (no per-row mul). */
    uint32_t *p = &g->vram[y * g->stride_px + x];
    for (uint32_t row = 0; row < h; row++) {
        gfx_fill_row(p, w, pix);
        p += g->stride_px;
    }
}
