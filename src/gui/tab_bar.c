/*
 * gui/tab_bar.c — Tab bar rendering and hit-testing for Terminator
 *
 * Draws a row of clickable tab rectangles at the top of the X11 window
 * and provides a simple point-in-rect test for dispatching mouse clicks.
 *
 * Compile with:  cc -Wall -Wextra -g -pthread -Iinclude -c src/gui/tab_bar.c
 */

#include "gui/tab_bar.h"
#include "gui/x11_window.h"   /* full struct GuiState definition       */
#include "gui/screen_buf.h"   /* ScreenBuf — we read screen->cwd       */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Geometry constants ───────────────────────────────────────────── */
#define TAB_PAD_Y        5    /* vertical padding above & below label  */
#define TAB_PAD_X        4    /* horizontal padding inside tab rect    */
#define TAB_GAP          2    /* horizontal gap between tab rects      */
#define MAX_LABEL_LEN    48   /* max chars we show in a tab label      */

/* ── Helper: abbreviate $HOME → '~' and optionally truncate ──────── */
static void build_label(char *buf, size_t bufsz, int display_num,
                        const char *cwd)
{
    const char *home = getenv("HOME");
    const char *path = cwd ? cwd : "?";
    char  shortened[MAX_PATH_LEN];

    /* Replace leading $HOME with '~' */
    if (home && strncmp(path, home, strlen(home)) == 0) {
        snprintf(shortened, sizeof(shortened), "~%s", path + strlen(home));
        path = shortened;
    }

    /*
     * Build the raw label:  " N: /path "
     * (leading/trailing space gives visual breathing room inside the rect)
     */
    char raw[MAX_PATH_LEN + 16];
    snprintf(raw, sizeof(raw), " %d: %s ", display_num, path);

    /* Truncate long labels with "…" so the tab bar stays usable */
    if (strlen(raw) > MAX_LABEL_LEN) {
        /* Keep the first (MAX_LABEL_LEN - 4) chars, then append "... " */
        raw[MAX_LABEL_LEN - 4] = '.';
        raw[MAX_LABEL_LEN - 3] = '.';
        raw[MAX_LABEL_LEN - 2] = '.';
        raw[MAX_LABEL_LEN - 1] = ' ';
        raw[MAX_LABEL_LEN]     = '\0';
    }

    snprintf(buf, bufsz, "%s", raw);
}

/* ══════════════════════════════════════════════════════════════════════
 * tab_bar_init
 * ══════════════════════════════════════════════════════════════════════ */
void tab_bar_init(TabBarState *tbs, int font_height)
{
    if (!tbs) return;

    tbs->bar_height = font_height + TAB_PAD_Y * 2;   /* 5px top + 5px bot */
    tbs->count      = 0;
    memset(tbs->rects, 0, sizeof(tbs->rects));
}

/* ══════════════════════════════════════════════════════════════════════
 * tab_bar_draw
 * ══════════════════════════════════════════════════════════════════════ */
void tab_bar_draw(struct GuiState *gui)
{
    if (!gui) return;

    Display      *dpy        = gui->display;
    Drawable      buf        = gui->backbuffer;
    GC            gc         = gui->gc;
    XFontStruct  *font       = gui->font;
    int           bar_h      = gui->tab_bar.bar_height;
    int           win_w      = gui->win_width;
    TabBarState  *tbs        = &gui->tab_bar;

    int rect_idx = 0;   /* running index into tbs->rects[] */

    /* ── 1. Fill the entire tab-bar background ────────────────────── */
    XSetForeground(dpy, gc, gui->color_tab_bg);
    XFillRectangle(dpy, buf, gc, 0, 0, (unsigned)win_w, (unsigned)bar_h);

    /* ── 2. Calculate [+] button width and available width ────────── */
    const char *plus_label = " + ";
    int plus_text_w = XTextWidth(font, plus_label, (int)strlen(plus_label));
    int plus_btn_w  = plus_text_w + TAB_PAD_X * 2;

    int active_tabs_count = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        if (gui->tabs[i].in_use) {
            active_tabs_count++;
        }
    }

    int has_free_slot = (active_tabs_count < MAX_TABS);

    /* Available width for all tab headers */
    int total_avail_w = win_w - (has_free_slot ? (plus_btn_w + TAB_GAP * 3) : (TAB_GAP * 2));
    int max_tab_w = 0;
    if (active_tabs_count > 0 && total_avail_w > 0) {
        max_tab_w = (total_avail_w / active_tabs_count) - TAB_GAP;
        if (max_tab_w < 20) {
            max_tab_w = 20; /* minimum width per tab */
        }
    }

    /* ── 3. Draw one rectangle per active tab ─────────────────────── */
    int x_cursor = TAB_GAP;          /* start a small gap from the left  */
    int display_num = 0;             /* 1-based number shown in the label*/

    for (int i = 0; i < MAX_TABS; i++) {
        if (!gui->tabs[i].in_use)
            continue;

        display_num++;

        /* Build the label string, e.g. " 1: ~/projects " */
        char label[MAX_LABEL_LEN + 1];
        const char *cwd = gui->tabs[i].screen ? gui->tabs[i].screen->cwd
                                               : NULL;
        build_label(label, sizeof(label), display_num, cwd);

        int text_w = XTextWidth(font, label, (int)strlen(label));
        int tab_w  = text_w + TAB_PAD_X * 2;

        /* Shrink tab width if tabs exceed window width */
        if (max_tab_w > 0 && tab_w > max_tab_w) {
            tab_w = max_tab_w;
        }

        int tab_h  = bar_h - 2;          /* 1px top margin + 1px for border */
        int tab_y  = 1;                  /* 1px below the window top edge   */

        /* Choose background colour */
        unsigned long bg = (i == gui->active_tab) ? gui->color_tab_active
                                                  : gui->color_tab_inactive;

        /* Draw tab rectangle fill */
        XSetForeground(dpy, gc, bg);
        XFillRectangle(dpy, buf, gc, x_cursor, tab_y,
                       (unsigned)tab_w, (unsigned)tab_h);

        /* Draw text label (vertically centred inside the tab rect) */
        int text_y = (bar_h + font->ascent - font->descent) / 2;
        XSetForeground(dpy, gc, gui->color_tab_text);

        /* Calculate how many characters fit in tab_w */
        int avail_text_w = tab_w - TAB_PAD_X * 2;
        if (avail_text_w > 0) {
            int draw_chars = (int)strlen(label);
            while (draw_chars > 0 && XTextWidth(font, label, draw_chars) > avail_text_w) {
                draw_chars--;
            }
            if (draw_chars > 0) {
                XDrawString(dpy, buf, gc,
                            x_cursor + TAB_PAD_X, text_y,
                            label, draw_chars);
            }
        }

        /* Subtle border around the tab */
        XSetForeground(dpy, gc, gui->color_border);
        XDrawRectangle(dpy, buf, gc, x_cursor, tab_y,
                       (unsigned)tab_w, (unsigned)tab_h);

        /* Record hit-test rect */
        if (rect_idx < MAX_TAB_RECTS) {
            tbs->rects[rect_idx].x      = x_cursor;
            tbs->rects[rect_idx].y      = tab_y;
            tbs->rects[rect_idx].w      = tab_w;
            tbs->rects[rect_idx].h      = tab_h;
            tbs->rects[rect_idx].action = i;   /* tab index */
            rect_idx++;
        }

        x_cursor += tab_w + TAB_GAP;
    }

    /* ── 4. Draw the [+] "new tab" button (only if slots are available) ── */
    if (has_free_slot) {
        int btn_w  = plus_btn_w;
        int btn_h  = bar_h - 2;
        int btn_y  = 1;

        /* Inactive style for the button */
        XSetForeground(dpy, gc, gui->color_tab_inactive);
        XFillRectangle(dpy, buf, gc, x_cursor, btn_y,
                       (unsigned)btn_w, (unsigned)btn_h);

        int text_y = (bar_h + font->ascent - font->descent) / 2;
        XSetForeground(dpy, gc, gui->color_tab_text);
        XDrawString(dpy, buf, gc,
                    x_cursor + TAB_PAD_X, text_y,
                    plus_label, (int)strlen(plus_label));

        XSetForeground(dpy, gc, gui->color_border);
        XDrawRectangle(dpy, buf, gc, x_cursor, btn_y,
                       (unsigned)btn_w, (unsigned)btn_h);

        /* Record hit-test rect for the [+] button */
        if (rect_idx < MAX_TAB_RECTS) {
            tbs->rects[rect_idx].x      = x_cursor;
            tbs->rects[rect_idx].y      = btn_y;
            tbs->rects[rect_idx].w      = btn_w;
            tbs->rects[rect_idx].h      = btn_h;
            tbs->rects[rect_idx].action = TAB_ACTION_NEW;
            rect_idx++;
        }
    }

    /* ── 4. Bottom border line across full width ──────────────────── */
    XSetForeground(dpy, gc, gui->color_border);
    XDrawLine(dpy, buf, gc, 0, bar_h - 1, win_w, bar_h - 1);

    /* ── 5. Store the total rect count ────────────────────────────── */
    tbs->count = rect_idx;
}

/* ══════════════════════════════════════════════════════════════════════
 * tab_bar_click — point-in-rect hit test
 * ══════════════════════════════════════════════════════════════════════ */
int tab_bar_click(TabBarState *tbs, int x, int y)
{
    if (!tbs) return TAB_ACTION_NONE;

    /* Click below the tab bar belongs to the terminal area */
    if (y >= tbs->bar_height)
        return TAB_ACTION_NONE;

    for (int i = 0; i < tbs->count; i++) {
        const TabRect *r = &tbs->rects[i];
        if (x >= r->x && x < r->x + r->w &&
            y >= r->y && y < r->y + r->h) {
            return r->action;
        }
    }

    return TAB_ACTION_NONE;
}
