/* ui — minimal menu / dialog primitives for HAL firmwares.
 *
 * Two backends, share the same API:
 *
 *   UART backend (always available)
 *     Renders ANSI-style: clear screen, draw items, reverse-video the
 *     cursor row. Reads arrow keys from the host terminal — RVVM's
 *     chardev_term puts the host TTY in raw mode for us. Works headless.
 *
 *   GFX backend (when gfx_t is wired)
 *     Renders into a gfx_text_t character grid on top of the framebuffer.
 *     Reads HID keyboard scan codes (i2c-hid via hid.h).
 *
 * A ui_t can have neither, one, or both backends active. With both:
 * each frame draws to *both* outputs and input from either source
 * advances the cursor. Useful for cores that boot with a graphics
 * window AND a UART log — the same menu appears in both places.
 *
 * What's here:
 *
 *   ui_init_uart / ui_init_gfx        opt-in backend wiring
 *   ui_menu_run                       drive a list-of-items menu until
 *                                     the user selects or backs out
 *   ui_confirm                        yes/no dialog
 *   ui_message                        modal info banner with [OK]
 *   ui_pick_file                      FatFs directory picker (HAL_FATFS only)
 *
 * Loop model: ui_menu_run is *blocking*. It's expected that emulation
 * is paused while a menu is up — typical pattern is: hotkey pauses
 * emulation, opens menu, menu returns, emulation resumes. Cores that
 * want menu-while-running can call the lower-level ui_menu_step in a
 * tight loop alongside their own work. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gfx.h"
#include "gfx_text.h"
#include "hid.h"

typedef struct {
    /* UART backend state — always present, just on/off. */
    bool             uart_enabled;

    /* GFX backend state — populated by ui_init_gfx. */
    bool             gfx_enabled;
    gfx_t           *gfx;
    gfx_text_t      *text;       /* caller-owned grid; ui draws into it */
    hid_keyboard_t  *kb;         /* polled for keyboard input */
} ui_t;

typedef struct {
    const char *label;       /* visible text, ASCII */
    int         id;          /* returned by ui_menu_run on select */
    bool        disabled;    /* non-selectable, drawn dimmed */
    const char *hint;        /* optional right-aligned suffix (e.g. file size) */
} ui_item_t;

typedef struct {
    const char *title;       /* drawn at top of menu, optional */
    ui_item_t  *items;
    uint32_t    count;
    uint32_t    selected;    /* mutable cursor — pre-set to default selection */
} ui_menu_t;

/* Backend wiring. UART path needs nothing; GFX path expects an
 * already-initialised gfx + gfx_text + hid_keyboard. */
bool ui_init_uart(ui_t *u);
bool ui_init_gfx(ui_t *u, gfx_t *gfx, gfx_text_t *text, hid_keyboard_t *kb);

/* Drive `menu` until the user picks an item or presses Esc.
 * Returns the picked item's `id` field, or -1 on Esc/back.
 * Disabled items are skipped during navigation. */
int  ui_menu_run(ui_t *u, ui_menu_t *menu);

/* Yes/no dialog. Returns true on Y / Enter, false on N / Esc. */
bool ui_confirm(ui_t *u, const char *prompt);

/* Modal info banner. Blocks until user presses any key. */
void ui_message(ui_t *u, const char *text);

#ifdef HAL_FATFS
/* FatFs directory picker. Lists files in `dir` (defaults to "/")
 * matching `ext` (e.g. "gb", "bas", or NULL for all). Writes the
 * selected absolute path into `out` and returns true. Returns false
 * if the user backs out or the directory can't be opened. */
bool ui_pick_file(ui_t *u, const char *dir, const char *ext,
                  char *out, size_t outsz);
#endif
