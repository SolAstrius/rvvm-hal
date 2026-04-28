/* Generic character-grid renderer on top of gfx.
 *
 * Common shape: a cols × rows grid of 1-byte character codes, an
 * optional 1-byte attribute per cell (for FG/BG color), a font
 * (1 bpp, 8 pixels wide × N pixels tall, 256 glyphs), and a target
 * gfx surface to render onto. Covers everything from Apple I's
 * 40×24 mono terminal through C64's 40×25 16-color text mode.
 *
 * Two layers of abstraction:
 *
 * 1. gfx_blit_glyph()
 *    Draw ONE glyph at a pixel position. Lowest-level, useful for
 *    UI overlays, custom layouts, debug printouts. Takes explicit
 *    fg/bg colors per call.
 *
 * 2. gfx_text_t + gfx_text_render()
 *    Maintain a grid (chars buffer + optional attrs buffer), render
 *    the whole thing to gfx in one call. The consumer either uses
 *    the simple "single fg/bg for everything" mono mode, or provides
 *    a 256-entry attr→(fg,bg) palette for per-cell color.
 *
 * The HAL ships no default font — consumers either bring their own
 * (Apple I's Signetics 2513 charset, Speccy's $3D00 ROM glyph table,
 * the MSX BIOS charset, ...) or use a sibling pd-font header. Font
 * data is just a const byte array, ownership-free.
 *
 * Font format: char_code N's glyph lives at font[N * cell_h .. N * cell_h + cell_h - 1].
 * Each byte is one row of pixels, MSB = leftmost pixel. Glyphs are
 * always 8 pixels wide; if your real glyph is 5×7 (Apple I) or 6×8
 * (Speccy), pad the unused right columns with zero. cell_h is the
 * vertical resolution. 8×8 and 8×16 are the common variants. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"

/* Render one 8-pixel-wide × cell_h-pixel-tall glyph at (x_px, y_px),
 * scaled by `scale` (1 = native, 2 = double, etc). FG pixels get
 * `fg_color`, BG pixels get `bg_color` — both XRGB8888. Bounds-clamped
 * against the surface's width/height. */
void gfx_blit_glyph(const gfx_t *g, const uint8_t *font, uint32_t cell_h,
                    uint8_t char_code,
                    uint32_t x_px, uint32_t y_px, uint32_t scale,
                    uint32_t fg_color, uint32_t bg_color);

/* Color decoder: given an attribute byte, fill *fg and *bg with
 * XRGB8888 colors. Caller-provided to make the same renderer work
 * for Speccy (BRIGHT+FLASH+ink+paper), CGA (FG_lo + BG_hi), C64 (16
 * fg colors, fixed bg), etc. Pass NULL on the gfx_text_t to use the
 * struct's default fg/bg for every cell (mono mode). */
typedef void (*gfx_text_attr_decode_t)(uint8_t attr,
                                       uint32_t *out_fg, uint32_t *out_bg,
                                       void *ctx);

/* Grid descriptor. Caller owns chars[] (cols*rows bytes) and optionally
 * attrs[] (also cols*rows). Pass attrs=NULL for mono grids. */
typedef struct {
    uint32_t cols, rows;
    uint32_t cell_h;             /* glyph height in pixels (8, 16, …) */
    uint32_t scale;              /* pixel scale on render (1, 2, 3, …) */
    const uint8_t        *font;  /* 256 * cell_h bytes */
    uint8_t              *chars; /* cols * rows */
    uint8_t              *attrs; /* cols * rows, or NULL */
    uint32_t              fg, bg;/* used when attrs == NULL or decoder is NULL */
    gfx_text_attr_decode_t attr_decode;
    void                 *attr_ctx;
} gfx_text_t;

/* Helper: zero-fill chars[] (and attrs[] if present, with attr_default). */
void gfx_text_clear(gfx_text_t *t, uint8_t attr_default);

/* Helper: write a NUL-terminated string at (col, row). Wraps within
 * the row only — does not auto-scroll. attr applied to all cells
 * touched (ignored if attrs == NULL). */
void gfx_text_puts(gfx_text_t *t, uint32_t col, uint32_t row,
                   const char *s, uint8_t attr);

/* Helper: scroll the grid up by `lines` rows. The bottom `lines`
 * rows are filled with space (0x20) characters and `attr_default`
 * for attrs[]. Used to implement teletype-style auto-scroll. */
void gfx_text_scroll_up(gfx_text_t *t, uint32_t lines, uint8_t attr_default);

/* Render the entire grid onto `g` with top-left at (x_off, y_off).
 * Each cell is 8 * scale pixels wide × cell_h * scale pixels tall.
 * Total rendered region is (cols * 8 * scale) × (rows * cell_h * scale). */
void gfx_text_render(const gfx_text_t *t, const gfx_t *g,
                     uint32_t x_off, uint32_t y_off);
