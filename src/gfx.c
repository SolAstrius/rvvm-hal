#include "gfx.h"
#include "bochs.h"
#include "fdt.h"
#include "uart.h"
#include "rvvm.h"
#include <stddef.h>

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

void gfx_fill(const gfx_t *g, uint32_t color) {
    if (g->backend == GFX_BACKEND_NONE) return;
    uint32_t pix = color;
    if (g->format == GFX_FMT_XBGR8888) {
        uint32_t r = (color >> 16) & 0xFF;
        uint32_t b = (color >>  0) & 0xFF;
        pix = (color & 0xFF00FF00U) | (b << 16) | r;
    }
    /* Row-by-row to honour a stride > width (simplefb may pad). */
    for (uint32_t y = 0; y < g->height; y++) {
        uint32_t *row = &g->vram[y * g->stride_px];
        for (uint32_t x = 0; x < g->width; x++) row[x] = pix;
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
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *p = &g->vram[(y + row) * g->stride_px + x];
        for (uint32_t col = 0; col < w; col++) p[col] = pix;
    }
}
