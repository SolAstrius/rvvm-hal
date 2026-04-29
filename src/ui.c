/* ui.c — menu / dialog primitives, UART + GFX backends.
 *
 * Single source for both rendering paths. Every draw walks the
 * enabled backends in turn; every input poll checks both keyboard
 * and UART, returning the first event that arrives. State is the
 * tiny ui_t struct — the actual menu data lives in caller storage.
 *
 * Layered:
 *   §1  Constants + ANSI escape strings
 *   §2  Input layer: poll UART (raw + ANSI escape decode) and HID
 *   §3  Render layer: draw a menu / dialog to UART and/or gfx_text
 *   §4  Public API: ui_init_*, ui_menu_run, ui_confirm, ui_message
 *   §5  FatFs directory picker (HAL_FATFS gated) */

#include "ui.h"
#include "uart.h"

/* HID + gfx are forward-declared in ui.h; the implementation
 * needs the real headers. */
#include "gfx.h"
#include "gfx_text.h"
#include "hid.h"

/* Stdlib-free helpers — keep this TU buildable without HAL_PICOLIBC. */
static inline unsigned long ui_strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

/* ====================================================================
 * §1. ANSI escape strings + key codes
 * ==================================================================== */

#define ANSI_CLEAR     "\033[2J\033[H"
#define ANSI_HOME      "\033[H"
#define ANSI_REVERSE   "\033[7m"
#define ANSI_DIM       "\033[2m"
#define ANSI_NORMAL    "\033[0m"
#define ANSI_HIDE_CUR  "\033[?25l"
#define ANSI_SHOW_CUR  "\033[?25h"

/* Logical keys returned by ui_poll_key. Encoded as small ints so
 * caller can switch() on them. ASCII printable chars pass through
 * verbatim. */
enum {
    UI_KEY_NONE  = 0,
    UI_KEY_UP    = 0x100,
    UI_KEY_DOWN  = 0x101,
    UI_KEY_LEFT  = 0x102,
    UI_KEY_RIGHT = 0x103,
    UI_KEY_ENTER = 0x104,
    UI_KEY_ESC   = 0x105,
    UI_KEY_BACK  = 0x106,    /* backspace */
};

/* USB HID usage codes — what hid.h's keyboard reports. Just the
 * keys ui cares about. */
#define HID_KEY_ENTER       0x28
#define HID_KEY_ESC         0x29
#define HID_KEY_BACKSPACE   0x2A
#define HID_KEY_RIGHT       0x4F
#define HID_KEY_LEFT        0x50
#define HID_KEY_DOWN        0x51
#define HID_KEY_UP          0x52

/* ====================================================================
 * §2. Input
 *
 * UART side: incremental ANSI-escape decoder. The host terminal
 * sends arrow keys as 3-byte sequences (\033 [ A/B/C/D). We track
 * a tiny state machine: 0 = neutral, 1 = saw \033, 2 = saw \033[.
 *
 * HID side: poll keyboard once, look at the press array for a key
 * matching one of the UI codes. We only track presses (not
 * releases) — menu navigation is press-only. To debounce, only
 * accept a key as "new" if it wasn't in the previous report.
 * ==================================================================== */

static int uart_esc_state = 0;

static int uart_decode_byte(uint8_t b) {
    switch (uart_esc_state) {
    case 0:
        if (b == 0x1B) { uart_esc_state = 1; return UI_KEY_NONE; }
        if (b == '\r' || b == '\n') return UI_KEY_ENTER;
        if (b == 0x7F || b == 0x08) return UI_KEY_BACK;
        if (b >= 0x20 && b < 0x7F) return (int)b;     /* printable */
        return UI_KEY_NONE;
    case 1:
        if (b == '[') { uart_esc_state = 2; return UI_KEY_NONE; }
        /* Lone ESC */
        uart_esc_state = 0;
        return UI_KEY_ESC;
    case 2:
        uart_esc_state = 0;
        switch (b) {
        case 'A': return UI_KEY_UP;
        case 'B': return UI_KEY_DOWN;
        case 'C': return UI_KEY_RIGHT;
        case 'D': return UI_KEY_LEFT;
        default:  return UI_KEY_NONE;
        }
    }
    uart_esc_state = 0;
    return UI_KEY_NONE;
}

static int uart_poll_key(void) {
    int c = uart_getc_nb();
    if (c < 0) return UI_KEY_NONE;
    return uart_decode_byte((uint8_t)c);
}

typedef struct {
    int  pressed;     /* the new-key callback writes here */
} hid_capture_t;

static void hid_capture_cb(uint8_t usage, bool pressed, void *ctx) {
    hid_capture_t *cap = ctx;
    if (!pressed || cap->pressed != 0) return;
    switch (usage) {
    case HID_KEY_UP:        cap->pressed = UI_KEY_UP;    break;
    case HID_KEY_DOWN:      cap->pressed = UI_KEY_DOWN;  break;
    case HID_KEY_LEFT:      cap->pressed = UI_KEY_LEFT;  break;
    case HID_KEY_RIGHT:     cap->pressed = UI_KEY_RIGHT; break;
    case HID_KEY_ENTER:     cap->pressed = UI_KEY_ENTER; break;
    case HID_KEY_ESC:       cap->pressed = UI_KEY_ESC;   break;
    case HID_KEY_BACKSPACE: cap->pressed = UI_KEY_BACK;  break;
    default: break;
    }
}

static int hid_poll_key(ui_t *u) {
    if (!u->kb) return UI_KEY_NONE;
    hid_capture_t cap = { 0 };
    hid_kb_poll(u->kb, hid_capture_cb, &cap);
    return cap.pressed;
}

/* Poll any backend's input. Returns the first key seen this tick. */
static int ui_poll_key(ui_t *u) {
    int k = UI_KEY_NONE;
    if (u->uart_enabled) k = uart_poll_key();
    if (k == UI_KEY_NONE && u->gfx_enabled) k = hid_poll_key(u);
    return k;
}

/* Block until any key arrives, then return it. */
static int ui_wait_key(ui_t *u) {
    for (;;) {
        int k = ui_poll_key(u);
        if (k != UI_KEY_NONE) return k;
    }
}

/* ====================================================================
 * §3. Render
 *
 * UART backend uses ANSI escapes; GFX backend uses gfx_text_puts +
 * gfx_text_render. Both render the same conceptual layout:
 *
 *     === <title> ===
 *
 *     > item 1
 *       item 2 (disabled-style)
 *       item 3                 [hint]
 *
 *     [↑↓ navigate, Enter select, Esc back]
 *
 * GFX backend assumes the caller-provided gfx_text_t is large
 * enough for the menu (e.g. 40 cols × 24 rows, the historical Apple
 * I size, fits 22 items + title + footer). Items past the grid
 * are clipped silently.
 * ==================================================================== */

static void render_uart(const ui_menu_t *menu) {
    uart_puts(ANSI_CLEAR ANSI_HIDE_CUR);
    if (menu->title) {
        uart_printf("=== %s ===\n\n", menu->title);
    }
    for (uint32_t i = 0; i < menu->count; i++) {
        const ui_item_t *it = &menu->items[i];
        bool sel = (i == menu->selected);
        if (sel)            uart_puts(ANSI_REVERSE);
        else if (it->disabled) uart_puts(ANSI_DIM);
        uart_printf(" %c %s",
                    sel ? '>' : ' ',
                    it->label ? it->label : "");
        if (it->hint) uart_printf("  [%s]", it->hint);
        uart_puts(ANSI_NORMAL "\n");
    }
    uart_puts("\n[arrows: navigate, Enter: select, Esc: back]\n");
}

static void render_gfx(ui_t *u, const ui_menu_t *menu) {
    if (!u->gfx_enabled || !u->text) return;
    gfx_text_clear(u->text, 0);
    uint32_t row = 0;
    if (menu->title) {
        char hdr[80];
        /* manual snprintf to keep this TU libc-free */
        unsigned i = 0;
        const char *open = "=== ";
        while (*open && i < sizeof(hdr) - 1) hdr[i++] = *open++;
        const char *t = menu->title;
        while (*t && i < sizeof(hdr) - 1)    hdr[i++] = *t++;
        const char *close = " ===";
        while (*close && i < sizeof(hdr) - 1) hdr[i++] = *close++;
        hdr[i] = 0;
        gfx_text_puts(u->text, 0, row++, hdr, 0);
        row++;     /* blank line */
    }
    for (uint32_t i = 0; i < menu->count && row < u->text->rows - 2; i++) {
        const ui_item_t *it = &menu->items[i];
        bool sel = (i == menu->selected);
        char marker[3] = { ' ', sel ? '>' : ' ', ' ' };
        gfx_text_puts(u->text, 0,           row, marker, 0);
        gfx_text_puts(u->text, 3,           row, it->label ? it->label : "", 0);
        if (it->hint) {
            uint32_t col = (uint32_t)ui_strlen(it->label) + 5;
            if (col < u->text->cols)
                gfx_text_puts(u->text, col, row, it->hint, 0);
        }
        row++;
    }
    /* footer */
    if (row + 1 < u->text->rows) {
        gfx_text_puts(u->text, 0, u->text->rows - 1,
                      "arrows | Enter | Esc", 0);
    }
    gfx_text_render(u->text, u->gfx, 0, 0);
}

static void render_menu(ui_t *u, const ui_menu_t *menu) {
    if (u->uart_enabled) render_uart(menu);
    if (u->gfx_enabled)  render_gfx(u, menu);
}

/* ====================================================================
 * §4. Public API
 * ==================================================================== */

bool ui_init_uart(ui_t *u) {
    if (!u) return false;
    u->uart_enabled = true;
    return true;
}

bool ui_init_gfx(ui_t *u, gfx_t *gfx, gfx_text_t *text, hid_keyboard_t *kb) {
    if (!u || !gfx || !text) return false;
    u->gfx_enabled = true;
    u->gfx  = gfx;
    u->text = text;
    u->kb   = kb;
    return true;
}

/* Find the next non-disabled index, wrapping. dir is +1 or -1. */
static uint32_t skip_disabled(const ui_menu_t *menu, uint32_t from, int dir) {
    if (menu->count == 0) return 0;
    uint32_t i = from;
    for (uint32_t step = 0; step < menu->count; step++) {
        if (!menu->items[i].disabled) return i;
        i = (dir > 0) ? (i + 1) % menu->count
                      : (i + menu->count - 1) % menu->count;
    }
    return from;     /* all disabled */
}

int ui_menu_run(ui_t *u, ui_menu_t *menu) {
    if (!u || !menu || menu->count == 0) return -1;

    /* Snap selection to a non-disabled item. */
    if (menu->selected >= menu->count) menu->selected = 0;
    menu->selected = skip_disabled(menu, menu->selected, +1);

    for (;;) {
        render_menu(u, menu);
        int k = ui_wait_key(u);
        switch (k) {
        case UI_KEY_UP:
            menu->selected = skip_disabled(menu,
                (menu->selected + menu->count - 1) % menu->count, -1);
            break;
        case UI_KEY_DOWN:
            menu->selected = skip_disabled(menu,
                (menu->selected + 1) % menu->count, +1);
            break;
        case UI_KEY_ENTER:
        case UI_KEY_RIGHT:
            if (!menu->items[menu->selected].disabled) {
                /* Restore terminal state on UART side before returning. */
                if (u->uart_enabled) uart_puts(ANSI_SHOW_CUR);
                return menu->items[menu->selected].id;
            }
            break;
        case UI_KEY_ESC:
        case UI_KEY_BACK:
            if (u->uart_enabled) uart_puts(ANSI_SHOW_CUR);
            return -1;
        default: break;
        }
    }
}

bool ui_confirm(ui_t *u, const char *prompt) {
    ui_item_t items[2] = {
        { .label = "No",  .id = 0 },
        { .label = "Yes", .id = 1 },
    };
    ui_menu_t menu = {
        .title = prompt, .items = items, .count = 2, .selected = 0,
    };
    int r = ui_menu_run(u, &menu);
    return r == 1;
}

void ui_message(ui_t *u, const char *text) {
    ui_item_t items[1] = {
        { .label = "OK", .id = 0 },
    };
    ui_menu_t menu = {
        .title = text, .items = items, .count = 1, .selected = 0,
    };
    (void)ui_menu_run(u, &menu);
}

/* ====================================================================
 * §5. FatFs directory picker
 *
 * Builds a menu of every regular file in `dir` matching `ext`.
 * Caps at 64 entries — enough for any realistic ROM library.
 * ==================================================================== */

#ifdef HAL_FATFS
#include "ff.h"
#include <string.h>      /* picolibc — HAL_FATFS implies HAL_PICOLIBC */
#include <stdio.h>

#define UI_PICK_MAX  64
#define UI_NAME_MAX  64

bool ui_pick_file(ui_t *u, const char *dir, const char *ext,
                  char *out, size_t outsz) {
    static char       names[UI_PICK_MAX][UI_NAME_MAX];
    static ui_item_t  items[UI_PICK_MAX];

    DIR    d;
    FILINFO fno;
    if (f_opendir(&d, dir ? dir : "/") != FR_OK) return false;

    int n = 0;
    size_t ext_len = ext ? strlen(ext) : 0;
    while (n < UI_PICK_MAX && f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        if (ext) {
            size_t fl = strlen(fno.fname);
            if (fl < ext_len + 1) continue;
            if (fno.fname[fl - ext_len - 1] != '.') continue;
            if (strcmp(&fno.fname[fl - ext_len], ext) != 0) continue;
        }
        size_t cl = strlen(fno.fname);
        if (cl >= UI_NAME_MAX) cl = UI_NAME_MAX - 1;
        memcpy(names[n], fno.fname, cl);
        names[n][cl] = 0;
        items[n].label    = names[n];
        items[n].id       = n;
        items[n].disabled = false;
        items[n].hint     = NULL;
        n++;
    }
    f_closedir(&d);

    if (n == 0) {
        ui_message(u, "(no files)");
        return false;
    }

    char title[80];
    snprintf(title, sizeof(title), "Pick file — %s", dir ? dir : "/");
    ui_menu_t menu = {
        .title = title, .items = items, .count = (uint32_t)n, .selected = 0,
    };
    int picked = ui_menu_run(u, &menu);
    if (picked < 0) return false;

    /* Compose absolute path. */
    const char *d_str = dir ? dir : "/";
    int sep = (d_str[0] && d_str[strlen(d_str) - 1] == '/') ? 0 : 1;
    snprintf(out, outsz, "%s%s%s",
             d_str, sep ? "/" : "", names[picked]);
    return true;
}
#endif /* HAL_FATFS */
