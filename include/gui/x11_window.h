/*
 * x11_window.h — Display, Window, GC, Font lifecycle and GUI state
 *
 * Owns the X11 display connection, window, graphics context, font,
 * back buffer, color scheme, tab array, and the main poll()-based
 * event loop that multiplexes X11 events with tab pipe I/O.
 */
#ifndef GUI_X11_WINDOW_H
#define GUI_X11_WINDOW_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "common.h"
#include "gui/screen_buf.h"
#include "gui/tab_bar.h"
#include "ipc/worker.h"
#include "ipc/shm_sync.h"

/* ── Default window dimensions ──────────────────────────────────────── */
#define DEFAULT_WIN_WIDTH   820
#define DEFAULT_WIN_HEIGHT  560

/* ── GUI state (the central structure for the entire GUI) ───────────── */
typedef struct GuiState {
    /* X11 objects */
    Display     *display;
    int          screen_num;
    Window       window;
    Pixmap       backbuffer;
    GC           gc;
    XFontStruct *font;
    Atom         wm_delete_msg;

    /* Window dimensions */
    int          win_width;
    int          win_height;

    /* Font metrics (monospace) */
    int          char_width;
    int          char_height;
    int          font_ascent;
    int          font_descent;

    /* Terminal grid (content area, excluding tab bar) */
    int          term_cols;
    int          term_rows;

    /* Color scheme */
    unsigned long color_bg;
    unsigned long color_fg;
    unsigned long color_tab_bg;
    unsigned long color_tab_active;
    unsigned long color_tab_inactive;
    unsigned long color_tab_text;
    unsigned long color_cursor;
    unsigned long color_prompt;
    unsigned long color_border;

    /* Tab bar */
    TabBarState  tab_bar;

    /* Tab array */
    struct {
        int         in_use;
        int         command_running;
        TabWorker   worker;
        ScreenBuf  *screen;
    } tabs[MAX_TABS];
    int          num_tabs;      /* count of active (in_use) tabs */
    int          active_tab;    /* index of the currently displayed tab */

    /* Execution mode & shared state */
    ExecMode     mode;
    SharedState *shared;

    /* Event loop control */
    int          running;
} GuiState;

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * Initialize the GUI: open X11 display, create window, load font,
 * allocate colors, create back buffer.
 * Returns 0 on success, -1 on failure.
 */
int gui_init(GuiState *gui, ExecMode mode, SharedState *shared);

/*
 * Clean up all GUI resources: close tabs, free pixmap, font, display.
 */
void gui_cleanup(GuiState *gui);

/*
 * Open a new tab: allocate ScreenBuf, spawn worker, set as active.
 * Returns the tab index on success, -1 on failure.
 */
int gui_open_tab(GuiState *gui);

/*
 * Close a tab by index: stop worker, free ScreenBuf, switch active tab.
 */
void gui_close_tab(GuiState *gui, int tab_index);

/*
 * Run the main event loop. Blocks until the window is closed or
 * all tabs are closed. Uses poll() on the X11 fd and tab pipe fds.
 */
void gui_run(GuiState *gui);

/*
 * Full redraw: render tab bar + content to back buffer, copy to window.
 */
void gui_redraw(GuiState *gui);

#endif /* GUI_X11_WINDOW_H */
