/*
 * gui/tab_bar.h — Tab bar rendering and hit-testing for Terminator
 *
 * The tab bar sits at the top of the X11 window and shows one clickable
 * rectangle per open tab, plus a [+] button to create new tabs.
 * Active tab is highlighted with a distinct background colour.
 *
 * This header deliberately forward-declares struct GuiState so that
 * translation units which only need the TabBarState type (or the
 * click-testing function) do not have to pull in the full x11_window.h.
 */
#ifndef GUI_TAB_BAR_H
#define GUI_TAB_BAR_H

#include <X11/Xlib.h>
#include "common.h"

/* ── Actions returned by tab_bar_click ─────────────────────────────── */
#define TAB_ACTION_NONE   -2   /* click fell outside every tab rect    */
#define TAB_ACTION_NEW    -1   /* click on the [+] "new tab" button    */
/* Positive values (0 … MAX_TABS-1) = index of the tab to switch to    */

/* ── Hit-test rectangle ───────────────────────────────────────────── */
typedef struct {
    int x, y, w, h;
    int action;   /* tab index (0..MAX_TABS-1), or TAB_ACTION_NEW */
} TabRect;

#define MAX_TAB_RECTS (MAX_TABS + 1)  /* room for all tabs + the [+] btn */

/* ── Per-window tab-bar state (embedded in GuiState) ──────────────── */
typedef struct {
    TabRect rects[MAX_TAB_RECTS];
    int     count;        /* number of rects currently stored           */
    int     bar_height;   /* pixel height of the tab bar                */
} TabBarState;

/* ── Forward declaration — full definition lives in gui/x11_window.h ─ */
struct GuiState;

/* ── Public API ───────────────────────────────────────────────────── */

/*
 * tab_bar_init — Initialise a TabBarState.
 *
 * Sets bar_height based on font metrics and zeros the rect list.
 * Call once after the X11 font has been loaded.
 *
 * @tbs          Pointer to the TabBarState to initialise.
 * @font_height  The pixel height of the monospace font (ascent+descent).
 */
void tab_bar_init(TabBarState *tbs, int font_height);

/*
 * tab_bar_draw — Render the tab bar into gui->backbuffer.
 *
 * Iterates over every active tab, draws a labelled rectangle for each,
 * appends the [+] button, and finishes with a bottom border line.
 * The rects[] array inside gui->tab_bar is rebuilt on every call so that
 * hit-testing always reflects the current layout.
 *
 * @gui  Pointer to the full GUI state (Display, GC, Pixmap, colours …).
 */
void tab_bar_draw(struct GuiState *gui);

/*
 * tab_bar_click — Hit-test a mouse click against the stored rects.
 *
 * Returns:
 *   TAB_ACTION_NONE  – click was outside every rect (or below the bar).
 *   TAB_ACTION_NEW   – user clicked the [+] button.
 *   0 … MAX_TABS-1   – index of the tab the user clicked.
 *
 * @tbs  Pointer to the (already-drawn) TabBarState.
 * @x    X coordinate of the click, window-relative.
 * @y    Y coordinate of the click, window-relative.
 */
int  tab_bar_click(TabBarState *tbs, int x, int y);

#endif /* GUI_TAB_BAR_H */
