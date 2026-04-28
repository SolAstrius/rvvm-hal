/* gfx_text — character-grid renderer on top of gfx. See gfx_text.h
 * for the API contract; implementation choices below.
 *
 * Single-glyph blit (gfx_blit_glyph) does direct vram writes when
 * possible: hoists g->vram + stride out of the per-pixel loop and
 * walks rows linearly. ~4× faster than calling gfx_pixel per pixel.
 *
 * Grid render walks (col, row), fans out attribute decode → fg/bg,
 * then calls the same gfx_blit_glyph. The per-glyph cost is small
 * (8 × cell_h × scale² pixel writes); for a 40×24 ×2 8×8 grid that's
 * 122,880 pixel writes per render — comparable to the Speccy's
 * full screen update and well under one frame's budget. */

#include "gfx_text.h"
#include <stddef.h>

void gfx_blit_glyph(const gfx_t *g, const uint8_t *font, uint32_t cell_h,
                    uint8_t char_code,
                    uint32_t x_px, uint32_t y_px, uint32_t scale,
                    uint32_t fg_color, uint32_t bg_color) {
    if (g->backend == GFX_BACKEND_NONE) return;
    if (scale == 0) scale = 1;

    uint32_t glyph_w = 8 * scale;
    uint32_t glyph_h = cell_h * scale;

    /* Bounds clamp — same shape as gfx_rect. If the glyph would
     * spill off the right or bottom edge, clip it. */
    uint32_t draw_w = glyph_w;
    uint32_t draw_h = glyph_h;
    if (x_px >= g->width || y_px >= g->height) return;
    if (x_px + draw_w > g->width)  draw_w = g->width  - x_px;
    if (y_px + draw_h > g->height) draw_h = g->height - y_px;

    /* Apply XBGR swizzle once — same trick as the Game Boy blit. */
    uint32_t fg = fg_color, bg = bg_color;
    if (g->format == GFX_FMT_XBGR8888) {
        uint32_t r = (fg >> 16) & 0xFF, b = fg & 0xFF;
        fg = (fg & 0xFF00FF00U) | (b << 16) | r;
        r = (bg >> 16) & 0xFF;  b = bg & 0xFF;
        bg = (bg & 0xFF00FF00U) | (b << 16) | r;
    }

    const uint8_t *glyph = font + (uint32_t)char_code * cell_h;
    uint32_t *vram   = g->vram;
    uint32_t  stride = g->stride_px;

    for (uint32_t gy = 0; gy < cell_h; gy++) {
        uint8_t row_bits = glyph[gy];
        for (uint32_t sy = 0; sy < scale; sy++) {
            uint32_t py = y_px + gy * scale + sy;
            if (py >= y_px + draw_h) break;
            uint32_t *row = &vram[py * stride + x_px];
            for (uint32_t gx = 0; gx < 8; gx++) {
                uint32_t color = (row_bits & (0x80U >> gx)) ? fg : bg;
                for (uint32_t sx = 0; sx < scale; sx++) {
                    uint32_t px = gx * scale + sx;
                    if (px >= draw_w) break;
                    row[px] = color;
                }
            }
        }
    }
}

void gfx_text_clear(gfx_text_t *t, uint8_t attr_default) {
    uint32_t n = t->cols * t->rows;
    for (uint32_t i = 0; i < n; i++) t->chars[i] = 0x20;
    if (t->attrs) {
        for (uint32_t i = 0; i < n; i++) t->attrs[i] = attr_default;
    }
}

void gfx_text_puts(gfx_text_t *t, uint32_t col, uint32_t row,
                   const char *s, uint8_t attr) {
    if (row >= t->rows) return;
    uint32_t off = row * t->cols + col;
    while (*s && col < t->cols) {
        t->chars[off] = (uint8_t)*s++;
        if (t->attrs) t->attrs[off] = attr;
        off++;
        col++;
    }
}

void gfx_text_scroll_up(gfx_text_t *t, uint32_t lines, uint8_t attr_default) {
    if (lines == 0) return;
    if (lines >= t->rows) {
        gfx_text_clear(t, attr_default);
        return;
    }
    uint32_t cells_kept   = (t->rows - lines) * t->cols;
    uint32_t shift        = lines * t->cols;
    /* Move chars[] up by `lines` rows. */
    for (uint32_t i = 0; i < cells_kept; i++)
        t->chars[i] = t->chars[i + shift];
    for (uint32_t i = cells_kept; i < t->rows * t->cols; i++)
        t->chars[i] = 0x20;
    if (t->attrs) {
        for (uint32_t i = 0; i < cells_kept; i++)
            t->attrs[i] = t->attrs[i + shift];
        for (uint32_t i = cells_kept; i < t->rows * t->cols; i++)
            t->attrs[i] = attr_default;
    }
}

void gfx_text_render(const gfx_text_t *t, const gfx_t *g,
                     uint32_t x_off, uint32_t y_off) {
    if (g->backend == GFX_BACKEND_NONE) return;
    uint32_t scale  = t->scale ? t->scale : 1;
    uint32_t glyph_w = 8 * scale;
    uint32_t glyph_h = t->cell_h * scale;

    for (uint32_t row = 0; row < t->rows; row++) {
        uint32_t py = y_off + row * glyph_h;
        for (uint32_t col = 0; col < t->cols; col++) {
            uint32_t off = row * t->cols + col;
            uint8_t  ch  = t->chars[off];

            uint32_t fg = t->fg;
            uint32_t bg = t->bg;
            if (t->attrs && t->attr_decode) {
                t->attr_decode(t->attrs[off], &fg, &bg, t->attr_ctx);
            }

            uint32_t px = x_off + col * glyph_w;
            gfx_blit_glyph(g, t->font, t->cell_h, ch,
                           px, py, scale, fg, bg);
        }
    }
}
