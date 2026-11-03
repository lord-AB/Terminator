/*
 * x11_window.c — X11 window management and main event loop
 *
 * Opens the display, creates a double-buffered window with a monospace font,
 * and runs a poll()-based event loop that multiplexes X11 events with
 * tab worker pipe I/O.  Zero busy-waiting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "gui/x11_window.h"
#include "gui/screen_buf.h"
#include "gui/tab_bar.h"
#include "gui/clipboard.h"
#include "input/line_editor.h"
#include "input/autocomplete.h"
#include "features/history_search.h"
#include "features/history_store.h"
#include "ipc/protocol.h"
#include "ipc/worker.h"
#include "parser/multiline.h"

/* ── Color helper ───────────────────────────────────────────────────── */
static unsigned long alloc_color(Display *dpy, int scr,
                                 unsigned char r, unsigned char g, unsigned char b) {
    XColor xc;
    xc.red   = (unsigned short)((r << 8) | r);
    xc.green = (unsigned short)((g << 8) | g);
    xc.blue  = (unsigned short)((b << 8) | b);
    xc.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, scr), &xc))
        return WhitePixel(dpy, scr);
    return xc.pixel;
}

/* ── Recalculate terminal grid from window size ─────────────────────── */
static void recalc_grid(GuiState *gui) {
    int content_height = gui->win_height - gui->tab_bar.bar_height;
    if (content_height < gui->char_height)
        content_height = gui->char_height;
    gui->term_rows = content_height / gui->char_height;
    gui->term_cols = (gui->win_width - 8) / gui->char_width;  /* 4px margin each side */
    if (gui->term_cols < 10) gui->term_cols = 10;
    if (gui->term_rows < 2)  gui->term_rows = 2;
}

/* ── Recreate the back buffer pixmap ────────────────────────────────── */
static void recreate_backbuffer(GuiState *gui) {
    if (gui->backbuffer)
        XFreePixmap(gui->display, gui->backbuffer);
    gui->backbuffer = XCreatePixmap(
        gui->display, gui->window,
        (unsigned)gui->win_width, (unsigned)gui->win_height,
        (unsigned)DefaultDepth(gui->display, gui->screen_num));
}

/* ══════════════════════════════════════════════════════════════════════
 *  GUI INIT / CLEANUP
 * ══════════════════════════════════════════════════════════════════════ */

int gui_init(GuiState *gui, ExecMode mode, SharedState *shared) {
    memset(gui, 0, sizeof(GuiState));
    gui->mode   = mode;
    gui->shared = shared;
    gui->active_tab = -1;

    /* Mark all tab slots as unused */
    for (int i = 0; i < MAX_TABS; i++) {
        gui->tabs[i].in_use = 0;
        worker_init(&gui->tabs[i].worker);
        gui->tabs[i].screen = NULL;
    }

    /* ── Open display ───────────────────────────────────────────────── */
    gui->display = XOpenDisplay(NULL);
    if (!gui->display) {
        TERM_LOG("gui_init: cannot open X11 display (is DISPLAY set?)");
        return -1;
    }
    gui->screen_num = DefaultScreen(gui->display);

    /* ── Load monospace font ────────────────────────────────────────── */
    const char *font_names[] = {
        "-misc-fixed-medium-r-normal--15-140-75-75-c-90-iso8859-1",
        "9x15",
        "-misc-fixed-medium-r-normal--14-*-*-*-c-*-iso8859-1",
        "fixed",
        NULL
    };
    gui->font = NULL;
    for (int i = 0; font_names[i]; i++) {
        gui->font = XLoadQueryFont(gui->display, font_names[i]);
        if (gui->font) {
            TERM_LOG("gui_init: loaded font \"%s\"", font_names[i]);
            break;
        }
    }
    if (!gui->font) {
        TERM_LOG("gui_init: cannot load any monospace font, using default");
        gui->font = XLoadQueryFont(gui->display, "fixed");
        if (!gui->font) {
            TERM_LOG("gui_init: cannot load even 'fixed' font, aborting");
            XCloseDisplay(gui->display);
            return -1;
        }
    }
    gui->font_ascent  = gui->font->ascent;
    gui->font_descent = gui->font->descent;
    gui->char_height  = gui->font_ascent + gui->font_descent;
    gui->char_width   = gui->font->max_bounds.width;

    /* ── Allocate colors ────────────────────────────────────────────── */
    gui->color_bg           = alloc_color(gui->display, gui->screen_num, 0x0C, 0x0C, 0x0C);
    gui->color_fg           = alloc_color(gui->display, gui->screen_num, 0xCC, 0xCC, 0xCC);
    gui->color_tab_bg       = alloc_color(gui->display, gui->screen_num, 0x1C, 0x1C, 0x1C);
    gui->color_tab_active   = alloc_color(gui->display, gui->screen_num, 0x33, 0x33, 0x33);
    gui->color_tab_inactive = alloc_color(gui->display, gui->screen_num, 0x22, 0x22, 0x22);
    gui->color_tab_text     = alloc_color(gui->display, gui->screen_num, 0xFF, 0xFF, 0xFF);
    gui->color_cursor       = alloc_color(gui->display, gui->screen_num, 0x00, 0xFF, 0x00);
    gui->color_prompt       = alloc_color(gui->display, gui->screen_num, 0x55, 0xFF, 0x55);
    gui->color_border       = alloc_color(gui->display, gui->screen_num, 0x55, 0x55, 0x55);

    /* ── Create window ──────────────────────────────────────────────── */
    gui->win_width  = DEFAULT_WIN_WIDTH;
    gui->win_height = DEFAULT_WIN_HEIGHT;

    gui->window = XCreateSimpleWindow(
        gui->display,
        RootWindow(gui->display, gui->screen_num),
        0, 0,
        (unsigned)gui->win_width, (unsigned)gui->win_height,
        0,
        gui->color_border,
        gui->color_bg);

    /* Set window title */
    XStoreName(gui->display, gui->window, "Terminator");

    /* Register for WM_DELETE_WINDOW so the close button works */
    gui->wm_delete_msg = XInternAtom(gui->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(gui->display, gui->window, &gui->wm_delete_msg, 1);

    /* Select events */
    XSelectInput(gui->display, gui->window,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                 ButtonReleaseMask | Button1MotionMask |
                 StructureNotifyMask | FocusChangeMask);

    /* Set the GC and font */
    gui->gc = XCreateGC(gui->display, gui->window, 0, NULL);
    XSetFont(gui->display, gui->gc, gui->font->fid);
    XSetForeground(gui->display, gui->gc, gui->color_fg);
    XSetBackground(gui->display, gui->gc, gui->color_bg);

    /* Size hints */
    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        hints->flags      = PMinSize;
        hints->min_width  = 320;
        hints->min_height = 200;
        XSetWMNormalHints(gui->display, gui->window, hints);
        XFree(hints);
    }

    /* ── Init tab bar ───────────────────────────────────────────────── */
    tab_bar_init(&gui->tab_bar, gui->char_height);

    /* ── Calculate grid ─────────────────────────────────────────────── */
    recalc_grid(gui);

    /* ── Create back buffer ─────────────────────────────────────────── */
    gui->backbuffer = 0;
    recreate_backbuffer(gui);

    /* ── Map window (make it visible) ───────────────────────────────── */
    XMapWindow(gui->display, gui->window);
    XFlush(gui->display);

    /* ── Initialize Clipboard & Hotkeys ─────────────────────────────── */
    clipboard_init(gui);
    memset(gui->cfg_err_msg, 0, sizeof(gui->cfg_err_msg));
    hotkey_cfg_load(&gui->hotkeys, NULL, gui->cfg_err_msg, sizeof(gui->cfg_err_msg));

    gui->running = 1;
    TERM_LOG("gui_init: window created (%dx%d, grid %dx%d)",
             gui->win_width, gui->win_height, gui->term_cols, gui->term_rows);
    return 0;
}

void gui_cleanup(GuiState *gui) {
    /* Close all tabs */
    for (int i = 0; i < MAX_TABS; i++) {
        if (gui->tabs[i].in_use)
            gui_close_tab(gui, i);
    }
    if (gui->backbuffer)
        XFreePixmap(gui->display, gui->backbuffer);
    if (gui->font)
        XFreeFont(gui->display, gui->font);
    if (gui->gc)
        XFreeGC(gui->display, gui->gc);
    if (gui->window)
        XDestroyWindow(gui->display, gui->window);
    if (gui->display)
        XCloseDisplay(gui->display);
    TERM_LOG("gui_cleanup: done");
}

/* ══════════════════════════════════════════════════════════════════════
 *  TAB MANAGEMENT
 * ══════════════════════════════════════════════════════════════════════ */

int gui_open_tab(GuiState *gui) {
    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!gui->tabs[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        TERM_LOG("gui_open_tab: no free tab slots");
        return -1;
    }

    /* Allocate screen buffer */
    gui->tabs[slot].screen = screen_buf_create();
    if (!gui->tabs[slot].screen) {
        TERM_LOG("gui_open_tab: screen_buf_create failed");
        return -1;
    }

    /* Spawn worker */
    worker_init(&gui->tabs[slot].worker);
    if (worker_spawn(&gui->tabs[slot].worker, slot, gui->mode, gui->shared) != TERM_OK) {
        TERM_LOG("gui_open_tab: worker_spawn failed for slot %d", slot);
        screen_buf_destroy(gui->tabs[slot].screen);
        gui->tabs[slot].screen = NULL;
        return -1;
    }

    gui->tabs[slot].in_use = 1;
    gui->tabs[slot].command_running = 0;
    gui->num_tabs++;
    gui->active_tab = slot;

    /* Report config errors in terminal if any */
    if (gui->cfg_err_msg[0] != '\0') {
        screen_buf_add_text(gui->tabs[slot].screen, gui->cfg_err_msg);
        gui->cfg_err_msg[0] = '\0';
    }

    TERM_LOG("gui_open_tab: opened tab %d (total %d)", slot, gui->num_tabs);
    return slot;
}

void gui_close_tab(GuiState *gui, int idx) {
    if (idx < 0 || idx >= MAX_TABS || !gui->tabs[idx].in_use)
        return;

    worker_stop(&gui->tabs[idx].worker);
    screen_buf_destroy(gui->tabs[idx].screen);
    gui->tabs[idx].screen = NULL;
    gui->tabs[idx].in_use = 0;
    gui->num_tabs--;

    TERM_LOG("gui_close_tab: closed tab %d (remaining %d)", idx, gui->num_tabs);

    /* Switch active tab if needed */
    if (gui->active_tab == idx) {
        gui->active_tab = -1;
        for (int i = 0; i < MAX_TABS; i++) {
            if (gui->tabs[i].in_use) {
                gui->active_tab = i;
                break;
            }
        }
    }

    /* If no tabs remain, signal exit */
    if (gui->num_tabs <= 0) {
        gui->running = 0;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  SELECTION AND RENDERING (with dynamic horizontal line wrapping)
 * ══════════════════════════════════════════════════════════════════════ */

static DisplayRow s_display_rows[MAX_DISPLAY_ROWS];

/*
 * compute_display_rows — dynamically wrap and lay out scrollback lines
 *                        and the active input line to the terminal width.
 */
int compute_display_rows(GuiState *gui, ScreenBuf *sb, DisplayRow *rows, int max_rows,
                        int *out_cursor_vr, int *out_cursor_vc) {
    if (!gui || !sb || !rows || max_rows <= 0) return 0;

    int cols = gui->term_cols > 0 ? gui->term_cols : 80;
    int total_rows = gui->term_rows > 0 ? gui->term_rows : 24;
    if (total_rows > max_rows) total_rows = max_rows;

    for (int r = 0; r < total_rows; r++) {
        rows[r].len = 0;
        rows[r].text[0] = '\0';
        rows[r].is_wrapped = 0;
        rows[r].is_prompt = 0;
        rows[r].is_input = 0;
        rows[r].logical_idx = -1;
        rows[r].sub_row = 0;
    }

    /* 1. Calculate input lines and visual rows */
    typedef struct {
        const char *prompt_str;
        int prompt_len;
        const char *text_ptr;
        int text_len;
        int in_start_offset;
        int vrows;
    } InputLineSegment;

    InputLineSegment in_segs[32];
    int num_segs = 0;
    int in_vrows = 0;

    const char *active_prompt = gui->tabs[gui->active_tab].command_running ? "" : sb->prompt;

    if (!strchr(sb->input, '\n')) {
        in_segs[0].prompt_str = active_prompt;
        in_segs[0].prompt_len = (int)strlen(active_prompt);
        in_segs[0].text_ptr = sb->input;
        in_segs[0].text_len = sb->input_len;
        in_segs[0].in_start_offset = 0;
        int seg_len = in_segs[0].prompt_len + in_segs[0].text_len;
        in_segs[0].vrows = (seg_len + cols - 1) / cols;
        if (in_segs[0].vrows < 1) in_segs[0].vrows = 1;
        in_vrows = in_segs[0].vrows;
        num_segs = 1;
    } else {
        const char *p = sb->input;
        int offset = 0;
        while (num_segs < 32) {
            const char *nl = strchr(p, '\n');
            int tlen = nl ? (int)(nl - p) : (int)strlen(p);
            in_segs[num_segs].prompt_str = (num_segs == 0) ? active_prompt : "> ";
            in_segs[num_segs].prompt_len = (int)strlen(in_segs[num_segs].prompt_str);
            in_segs[num_segs].text_ptr = p;
            in_segs[num_segs].text_len = tlen;
            in_segs[num_segs].in_start_offset = offset;

            int seg_len = in_segs[num_segs].prompt_len + in_segs[num_segs].text_len;
            int v = (seg_len + cols - 1) / cols;
            if (v < 1) v = 1;
            in_segs[num_segs].vrows = v;
            in_vrows += v;
            num_segs++;

            offset += tlen;
            if (nl) {
                offset++; /* skip '\n' */
                p = nl + 1;
            } else {
                break;
            }
        }
        if (in_vrows < 1) in_vrows = 1;
    }
    if (in_vrows > total_rows) in_vrows = total_rows;

    int has_autocomplete = (sb->autocomplete.in_select_mode && sb->autocomplete.menu_text[0] != '\0') ? 1 : 0;

    /* 2. Count total scrollback visual rows */
    int stored = screen_buf_visible_count(sb);
    int total_sb_vrows = 0;
    int idx = 0;
    while (idx < stored) {
        int total_chars = 0;
        while (idx < stored) {
            const char *l = screen_buf_get_line(sb, idx);
            if (l) total_chars += (int)strlen(l);
            int wrapped = screen_buf_get_line_wrapped(sb, idx);
            idx++;
            if (!wrapped) break;
        }
        int v = (total_chars + cols - 1) / cols;
        if (v < 1) v = 1;
        total_sb_vrows += v;
    }

    /* 3. Determine visual rows allocated to scrollback */
    int max_sb_rows = total_rows - in_vrows - has_autocomplete;
    if (max_sb_rows < 0) max_sb_rows = 0;

    int max_scroll = (total_sb_vrows > max_sb_rows) ? (total_sb_vrows - max_sb_rows) : 0;
    if (sb->scroll_offset > max_scroll) {
        sb->scroll_offset = max_scroll;
    }

    int end_vrow = total_sb_vrows - sb->scroll_offset;
    int start_vrow = end_vrow - max_sb_rows;
    if (start_vrow < 0) start_vrow = 0;

    int input_start_vr = 0;
    if (total_sb_vrows < max_sb_rows && sb->scroll_offset == 0) {
        input_start_vr = total_sb_vrows;
    } else {
        input_start_vr = end_vrow - start_vrow;
    }
    if (input_start_vr + in_vrows > total_rows) {
        input_start_vr = total_rows - in_vrows;
    }
    if (input_start_vr < 0) input_start_vr = 0;

    /* 4. Populate scrollback visual rows */
    int cur_vrow = 0;
    idx = 0;
    while (idx < stored) {
        int start_slot = idx;
        int end_slot = idx;
        int total_chars = 0;
        while (idx < stored) {
            const char *l = screen_buf_get_line(sb, idx);
            if (l) total_chars += (int)strlen(l);
            end_slot = idx;
            int wrapped = screen_buf_get_line_wrapped(sb, idx);
            idx++;
            if (!wrapped) break;
        }

        int v = (total_chars + cols - 1) / cols;
        if (v < 1) v = 1;

        for (int s = 0; s < v; s++) {
            if (cur_vrow >= start_vrow && cur_vrow < end_vrow) {
                int dest_vr = cur_vrow - start_vrow;
                if (dest_vr >= 0 && dest_vr < total_rows && dest_vr < input_start_vr) {
                    int char_start = s * cols;
                    int char_len = cols;
                    if (char_start + char_len > total_chars) {
                        char_len = total_chars - char_start;
                    }
                    if (char_len < 0) char_len = 0;

                    int copied = 0;
                    int skip = char_start;
                    for (int j = start_slot; j <= end_slot && copied < char_len; j++) {
                        const char *slot_str = screen_buf_get_line(sb, j);
                        int slot_len = slot_str ? (int)strlen(slot_str) : 0;
                        if (skip >= slot_len) {
                            skip -= slot_len;
                            continue;
                        }
                        int take = slot_len - skip;
                        if (take > char_len - copied) {
                            take = char_len - copied;
                        }
                        memcpy(&rows[dest_vr].text[copied], &slot_str[skip], (size_t)take);
                        copied += take;
                        skip = 0;
                    }
                    rows[dest_vr].text[copied] = '\0';
                    rows[dest_vr].len = copied;
                    rows[dest_vr].is_prompt = 0;
                    rows[dest_vr].is_input = 0;
                    rows[dest_vr].logical_idx = start_slot;
                    rows[dest_vr].sub_row = s;
                    rows[dest_vr].is_wrapped = (s < v - 1) ? 1 : 0;
                    if (s == v - 1 && end_slot == stored - 1 && screen_buf_get_line_wrapped(sb, end_slot)) {
                        rows[dest_vr].is_wrapped = 1;
                    }
                }
            }
            cur_vrow++;
        }
    }

    /* 5. Populate input visual rows and calculate cursor position */
    int cur_in_vr = input_start_vr;
    *out_cursor_vr = input_start_vr;
    *out_cursor_vc = 0;

    for (int seg = 0; seg < num_segs; seg++) {
        char seg_full[MAX_CMD_LEN + 256];
        int p_len = in_segs[seg].prompt_len;
        int t_len = in_segs[seg].text_len;
        snprintf(seg_full, sizeof(seg_full), "%.*s%.*s",
                 p_len, in_segs[seg].prompt_str,
                 t_len, in_segs[seg].text_ptr);
        int seg_total_len = p_len + t_len;

        for (int s = 0; s < in_segs[seg].vrows; s++) {
            int dest_vr = cur_in_vr + s;
            if (dest_vr >= total_rows) break;

            int char_start = s * cols;
            int char_len = cols;
            if (char_start + char_len > seg_total_len) char_len = seg_total_len - char_start;
            if (char_len < 0) char_len = 0;

            if (char_len > 0) {
                memcpy(rows[dest_vr].text, &seg_full[char_start], (size_t)char_len);
            }
            rows[dest_vr].text[char_len] = '\0';
            rows[dest_vr].len = char_len;

            int p_chars = 0;
            if (char_start < p_len) {
                p_chars = p_len - char_start;
                if (p_chars > char_len) p_chars = char_len;
            }
            rows[dest_vr].is_prompt = p_chars;
            rows[dest_vr].is_input = 1;
            rows[dest_vr].logical_idx = -1;
            rows[dest_vr].sub_row = s;
            rows[dest_vr].is_wrapped = (s < in_segs[seg].vrows - 1) ? 1 : 0;
        }

        /* Check if cursor falls in this segment */
        int seg_start = in_segs[seg].in_start_offset;
        int seg_end = seg_start + in_segs[seg].text_len;
        if (sb->cursor_pos >= seg_start && (sb->cursor_pos <= seg_end || seg == num_segs - 1)) {
            int c_off = sb->cursor_pos - seg_start;
            if (c_off > in_segs[seg].text_len) c_off = in_segs[seg].text_len;
            int c_idx = p_len + c_off;
            int c_sub_r = c_idx / cols;
            int c_col = c_idx % cols;
            if (c_sub_r >= in_segs[seg].vrows) {
                c_sub_r = in_segs[seg].vrows - 1;
                c_col = cols - 1;
            }
            *out_cursor_vr = cur_in_vr + c_sub_r;
            *out_cursor_vc = c_col;
        }

        cur_in_vr += in_segs[seg].vrows;
    }

    return total_rows;
}

void gui_clear_selection(GuiState *gui) {
    if (!gui) return;
    if (gui->has_selection || gui->is_selecting) {
        gui->has_selection = 0;
        gui->is_selecting = 0;
        TERM_LOG("gui_clear_selection: cleared selection");
        gui_redraw(gui);
    }
}

void gui_copy_selection(GuiState *gui) {
    if (!gui || !gui->has_selection) return;
    if (gui->active_tab < 0 || !gui->tabs[gui->active_tab].in_use) return;

    ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
    if (!sb) return;

    int cur_vr = 0, cur_vc = 0;
    int num_rows = compute_display_rows(gui, sb, s_display_rows, MAX_DISPLAY_ROWS, &cur_vr, &cur_vc);

    int r0 = gui->sel_start_row, c0 = gui->sel_start_col;
    int r1 = gui->sel_end_row,   c1 = gui->sel_end_col;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr = r0; r0 = r1; r1 = tr;
        int tc = c0; c0 = c1; c1 = tc;
    }

    if (r0 < 0) r0 = 0;
    if (r1 >= num_rows) r1 = num_rows - 1;

    char sel_buf[CLIPBOARD_MAX];
    size_t sel_len = 0;
    sel_buf[0] = '\0';

    for (int r = r0; r <= r1; r++) {
        int len = s_display_rows[r].len;
        int col_start = (r == r0) ? c0 : 0;
        int col_end   = (r == r1) ? c1 : (len > 0 ? len - 1 : 0);
        if (col_end >= len) col_end = len - 1;

        if (col_start < len && col_end >= col_start) {
            int chunk_len = col_end - col_start + 1;
            if (sel_len + (size_t)chunk_len < sizeof(sel_buf) - 2) {
                memcpy(&sel_buf[sel_len], &s_display_rows[r].text[col_start], (size_t)chunk_len);
                sel_len += (size_t)chunk_len;
            }
        }

        if (r < r1 && sel_len < sizeof(sel_buf) - 2) {
            /* If this row is soft-wrapped into next, don't insert newline */
            if (!s_display_rows[r].is_wrapped) {
                sel_buf[sel_len++] = '\n';
            }
        }
    }
    sel_buf[sel_len] = '\0';

    if (sel_len > 0) {
        clipboard_copy(gui, sel_buf, sel_len);
        TERM_LOG("gui_copy_selection: copied %zu bytes to clipboard", sel_len);
    }
}

void gui_redraw(GuiState *gui) {
    if (!gui->display || !gui->backbuffer)
        return;

    Display *dpy = gui->display;
    Pixmap   buf = gui->backbuffer;
    GC       gc  = gui->gc;

    /* Clear back buffer */
    XSetForeground(dpy, gc, gui->color_bg);
    XFillRectangle(dpy, buf, gc, 0, 0,
                   (unsigned)gui->win_width, (unsigned)gui->win_height);

    /* Draw tab bar */
    tab_bar_draw(gui);

    /* Draw content area for active tab */
    if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
        ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
        int bar_h     = gui->tab_bar.bar_height;
        int x_margin  = 4;

        int cur_vr = 0, cur_vc = 0;
        int num_rows = compute_display_rows(gui, sb, s_display_rows, MAX_DISPLAY_ROWS, &cur_vr, &cur_vc);

        /* Selection range normalization */
        int is_sel = gui->has_selection;
        int r0 = gui->sel_start_row, c0 = gui->sel_start_col;
        int r1 = gui->sel_end_row,   c1 = gui->sel_end_col;
        if (r0 > r1 || (r0 == r1 && c0 > c1)) {
            int tr = r0; r0 = r1; r1 = tr;
            int tc = c0; c0 = c1; c1 = tc;
        }

        for (int r = 0; r < num_rows; r++) {
            if (s_display_rows[r].len == 0) continue;

            int y_box  = bar_h + r * gui->char_height;
            int y_text = y_box + gui->font_ascent;
            int row_selected = (is_sel && r >= r0 && r <= r1);

            int c_sel_start = (r == r0) ? c0 : 0;
            int c_sel_end   = (r == r1) ? c1 : gui->term_cols;

            if (!row_selected) {
                /* Render normal row */
                if (s_display_rows[r].is_prompt > 0) {
                    /* Prompt segment */
                    XSetForeground(dpy, gc, gui->color_prompt);
                    XDrawString(dpy, buf, gc, x_margin, y_text, s_display_rows[r].text, s_display_rows[r].is_prompt);
                    /* Input segment */
                    if (s_display_rows[r].len > s_display_rows[r].is_prompt) {
                        int in_x = x_margin + s_display_rows[r].is_prompt * gui->char_width;
                        XSetForeground(dpy, gc, gui->color_fg);
                        XDrawString(dpy, buf, gc, in_x, y_text,
                                    s_display_rows[r].text + s_display_rows[r].is_prompt,
                                    s_display_rows[r].len - s_display_rows[r].is_prompt);
                    }
                } else {
                    XSetForeground(dpy, gc, gui->color_fg);
                    XDrawString(dpy, buf, gc, x_margin, y_text, s_display_rows[r].text, s_display_rows[r].len);
                }
            } else {
                /* Render row with inverted selection highlighting */
                for (int c = 0; c < s_display_rows[r].len; c++) {
                    int cx = x_margin + c * gui->char_width;
                    int is_prompt_char = (c < s_display_rows[r].is_prompt);
                    unsigned long fg_color = is_prompt_char ? gui->color_prompt : gui->color_fg;

                    if (c >= c_sel_start && c <= c_sel_end) {
                        /* Inverted highlight */
                        XSetForeground(dpy, gc, fg_color);
                        XFillRectangle(dpy, buf, gc, cx, y_box, (unsigned)gui->char_width, (unsigned)gui->char_height);
                        XSetForeground(dpy, gc, gui->color_bg);
                        XDrawString(dpy, buf, gc, cx, y_text, &s_display_rows[r].text[c], 1);
                    } else {
                        XSetForeground(dpy, gc, fg_color);
                        XDrawString(dpy, buf, gc, cx, y_text, &s_display_rows[r].text[c], 1);
                    }
                }
            }
        }

        /* ── Draw cursor (block) ─────────────────────────────────────────── */
        if (cur_vr >= 0 && cur_vr < num_rows) {
            int cursor_x = x_margin + cur_vc * gui->char_width;
            int cursor_y = bar_h + cur_vr * gui->char_height;
            int cursor_text_y = cursor_y + gui->font_ascent;

            XSetForeground(dpy, gc, gui->color_cursor);
            XFillRectangle(dpy, buf, gc, cursor_x, cursor_y,
                           (unsigned)gui->char_width, (unsigned)gui->char_height);

            /* Draw character under cursor in background color (inverted) */
            if (cur_vc < s_display_rows[cur_vr].len) {
                XSetForeground(dpy, gc, gui->color_bg);
                XDrawString(dpy, buf, gc, cursor_x, cursor_text_y,
                            &s_display_rows[cur_vr].text[cur_vc], 1);
            }
        }

        /* ── Autocomplete suggestions displayed directly under cursor ──── */
        if (sb->autocomplete.in_select_mode && sb->autocomplete.menu_text[0] != '\0') {
            int menu_len = (int)strlen(sb->autocomplete.menu_text);
            int menu_w   = menu_len * gui->char_width + 8;
            int cursor_x = x_margin + cur_vc * gui->char_width;
            int cursor_y = bar_h + cur_vr * gui->char_height;
            int menu_x   = cursor_x;
            if (menu_x + menu_w > gui->win_width - x_margin) {
                menu_x = gui->win_width - x_margin - menu_w;
            }
            if (menu_x < x_margin) {
                menu_x = x_margin;
            }

            int suggest_box_y = cursor_y + gui->char_height;
            if (suggest_box_y + gui->char_height > gui->win_height) {
                suggest_box_y = cursor_y - gui->char_height;
            }
            int suggest_text_y = suggest_box_y + gui->font_ascent;

            /* Background box with border */
            XSetForeground(dpy, gc, gui->color_tab_inactive);
            XFillRectangle(dpy, buf, gc, menu_x, suggest_box_y,
                           (unsigned)menu_w, (unsigned)gui->char_height);
            XSetForeground(dpy, gc, gui->color_border);
            XDrawRectangle(dpy, buf, gc, menu_x, suggest_box_y,
                           (unsigned)menu_w, (unsigned)gui->char_height);

            /* Text in prompt color */
            int max_chars = (gui->win_width - menu_x - 8) / gui->char_width;
            int draw_len  = (menu_len > max_chars && max_chars > 0) ? max_chars : menu_len;
            XSetForeground(dpy, gc, gui->color_prompt);
            XDrawString(dpy, buf, gc, menu_x + 4, suggest_text_y,
                        sb->autocomplete.menu_text, draw_len);
        }

        /* If scrolled up, draw scroll indicator at top right */
        if (sb->scroll_offset > 0) {
            char scroll_info[64];
            int slen = snprintf(scroll_info, sizeof(scroll_info),
                                "[Scroll: %d up | Shift+Down to bottom]", sb->scroll_offset);
            int badge_x = gui->win_width - (slen + 1) * gui->char_width - 8;
            if (badge_x > 0) {
                XSetForeground(dpy, gc, gui->color_prompt);
                XDrawString(dpy, buf, gc, badge_x, bar_h + gui->font_ascent, scroll_info, slen);
            }
        }
    } else {
        /* No active tab — show a message */
        const char *msg = "No tabs open. Press Ctrl+T to create one.";
        int len = (int)strlen(msg);
        int x = (gui->win_width - len * gui->char_width) / 2;
        int y = gui->win_height / 2;
        XSetForeground(dpy, gc, gui->color_fg);
        XDrawString(dpy, buf, gc, x, y, msg, len);
    }

    /* Copy back buffer to window */
    XCopyArea(dpy, buf, gui->window, gc, 0, 0,
              (unsigned)gui->win_width, (unsigned)gui->win_height, 0, 0);
    XFlush(dpy);
}

/* ══════════════════════════════════════════════════════════════════════
 *  EVENT HANDLERS
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Handle pipe data from a tab worker ─────────────────────────────── */
static void handle_tab_output(GuiState *gui, int tab_idx) {
    char payload[PIPE_BUF_SIZE];
    uint32_t msg_type, msg_len;

    int fd = gui->tabs[tab_idx].worker.gui_read_fd;
    if (fd < 0) return;

    if (proto_recv(fd, &msg_type, payload, sizeof(payload) - 1, &msg_len) < 0) {
        /* Pipe closed or error — tab likely exited */
        TERM_LOG("tab[%d]: pipe read error, closing tab", tab_idx);
        gui_close_tab(gui, tab_idx);
        gui_redraw(gui);
        return;
    }
    payload[msg_len] = '\0';

    ScreenBuf *sb = gui->tabs[tab_idx].screen;
    if (!sb) return;

    switch (msg_type) {
    case TAB_MSG_OUTPUT:
        screen_buf_add_text(sb, payload);
        break;
    case TAB_MSG_PROMPT:
        screen_buf_set_prompt(sb, payload);
        break;
    case TAB_MSG_CWD:
        screen_buf_set_cwd(sb, payload);
        break;
    case TAB_MSG_CLEAR:
        screen_buf_clear(sb);
        break;
    case TAB_MSG_EXIT:
        TERM_LOG("tab[%d]: received EXIT message", tab_idx);
        gui_close_tab(gui, tab_idx);
        break;
    case TAB_MSG_READY:
        gui->tabs[tab_idx].command_running = 0;
        TERM_LOG("tab[%d]: ready", tab_idx);
        break;
    default:
        TERM_LOG("tab[%d]: unknown message type %u", tab_idx, msg_type);
        break;
    }

    /* Redraw if output belongs to active tab, or if tab bar/state changed */
    if (tab_idx == gui->active_tab || msg_type == TAB_MSG_CWD || msg_type == TAB_MSG_EXIT) {
        gui_redraw(gui);
    }
}

/* ── Handle keyboard input ──────────────────────────────────────────── */
static void handle_keypress(GuiState *gui, XKeyEvent *ev) {
    char   buf[32];
    KeySym keysym = 0;
    int    len = XLookupString(ev, buf, sizeof(buf) - 1, &keysym, NULL);
    buf[len] = '\0';

    if (IsModifierKey(keysym))
        return;

    int ctrl  = (ev->state & ControlMask) != 0;
    int shift = (ev->state & ShiftMask) != 0;
    int alt   = (ev->state & Mod1Mask) != 0;
    int cmd   = (ev->state & (Mod4Mask | Mod2Mask)) != 0;
    UNUSED(shift);

    TERM_LOG("keypress: keysym=0x%lx ('%s') state=0x%x ctrl=%d cmd=%d alt=%d shift=%d",
             (unsigned long)keysym, len > 0 ? buf : "", ev->state, ctrl, cmd, alt, shift);

    /* ── Cmd+W: close current tab ──────────────────────────────────── */
    if (cmd && (keysym == XK_w || keysym == XK_W)) {
        if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
            TERM_LOG("tab[%d]: Cmd+W pressed, closing tab", gui->active_tab);
            gui_close_tab(gui, gui->active_tab);
            gui_redraw(gui);
            return;
        }
    }

    /* ── Ctrl+C: cancel current line / multiline input or interrupt job ── */
    if (ctrl && (keysym == XK_c || keysym == XK_C)) {
        if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
            ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
            int fd = gui->tabs[gui->active_tab].worker.gui_write_fd;
            if (fd >= 0) {
                int sig = SIGINT;
                proto_send(fd, GUI_MSG_SIGNAL, &sig, sizeof(sig));
            }
            if (sb) {
                autocomplete_cancel(&sb->autocomplete);
                if (sb->in_search) {
                    sb->in_search = 0;
                    screen_buf_set_prompt(sb, sb->saved_prompt[0] ? sb->saved_prompt : get_default_prompt_str());
                    screen_buf_input_clear(sb);
                    for (int i = 0; sb->saved_input[i]; i++) {
                        screen_buf_input_insert(sb, sb->saved_input[i]);
                    }
                } else if (sb->input_len > 0 || sb->in_multiline || !gui->tabs[gui->active_tab].command_running) {
                    char line[MAX_CMD_LEN + 32];
                    snprintf(line, sizeof(line), "%s%s^C", sb->prompt, sb->input);
                    screen_buf_add_line(sb, line);
                    screen_buf_input_clear(sb);
                    sb->in_multiline = 0;
                    sb->multiline_buf[0] = '\0';
                    screen_buf_set_prompt(sb, get_default_prompt_str());
                }
                screen_buf_scroll_reset(sb);
                gui_redraw(gui);
            }
        }
        return;
    }

    /* ── Ctrl+Z: suspend current foreground job ─────────────────────── */
    if (ctrl && (keysym == XK_z || keysym == XK_Z)) {
        if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
            int fd = gui->tabs[gui->active_tab].worker.gui_write_fd;
            if (fd >= 0) {
                int sig = SIGTSTP;
                proto_send(fd, GUI_MSG_SIGNAL, &sig, sizeof(sig));
            }
        }
        return;
    }

    /* ── Ctrl+D: EOF on running command or exit tab if prompt is empty ── */
    if (ctrl && (keysym == XK_d || keysym == XK_D)) {
        if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
            ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
            int fd = gui->tabs[gui->active_tab].worker.gui_write_fd;
            if (gui->tabs[gui->active_tab].command_running) {
                if (fd >= 0) {
                    proto_send(fd, GUI_MSG_EOF, NULL, 0);
                }
            } else if (sb && sb->input_len == 0 && !sb->in_multiline) {
                /* Empty line at prompt -> exit tab */
                TERM_LOG("tab[%d]: Ctrl+D on empty prompt, closing tab", gui->active_tab);
                gui_close_tab(gui, gui->active_tab);
                gui_redraw(gui);
            } else if (sb && sb->input_len > 0) {
                screen_buf_input_delete_forward(sb);
                gui_redraw(gui);
            }
        }
        return;
    }

    /* ── Ctrl+T / Cmd+T: new tab ───────────────────────────────────── */
    if ((ctrl || cmd) && (keysym == XK_t || keysym == XK_T)) {
        TERM_LOG("shortcut new tab requested (ctrl=%d, cmd=%d)", ctrl, cmd);
        gui_open_tab(gui);
        gui_redraw(gui);
        return;
    }

    /* ── Alt+1..9 / Cmd+1..9 / Ctrl+1..9: switch tabs ───────────────── */
    if ((cmd || alt || ctrl) && keysym >= XK_1 && keysym <= XK_9) {
        int target_num = (int)(keysym - XK_1);  /* 0-based */
        int count = 0;
        for (int i = 0; i < MAX_TABS; i++) {
            if (gui->tabs[i].in_use) {
                if (count == target_num) {
                    gui->active_tab = i;
                    TERM_LOG("tab switch: switched to tab %d (slot %d) via shortcut", count + 1, i);
                    gui_redraw(gui);
                    return;
                }
                count++;
            }
        }
        return;
    }

    /* ── No active tab → ignore other keys ──────────────────────────── */
    if (gui->active_tab < 0 || !gui->tabs[gui->active_tab].in_use)
        return;

    ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
    if (!sb) return;

    /* ── Autocomplete numbered menu selection mode ──────────────────── */
    if (sb->autocomplete.in_select_mode) {
        char ch = (len > 0) ? buf[0] : 0;
        if (autocomplete_handle_key(sb, keysym, ch)) {
            gui_redraw(gui);
            return;
        }
        /* If autocomplete_handle_key returned 0, autocomplete_cancel was called
         * and the suggestions are cleared immediately. We fall through so that
         * whatever key was pressed (a letter, backspace, etc.) is processed normally. */
    }

    /* ── Configurable Hotkeys (line_editor & clipboard actions) ─────── */
    HotkeyAction action = hotkey_cfg_match(&gui->hotkeys, keysym, ev->state);
    if (action != HOTKEY_ACTION_NONE) {
        if (line_editor_handle_action(gui, sb, action)) {
            gui_redraw(gui);
            return;
        }
    }

    switch (keysym) {
    case XK_Escape:
        if (gui->has_selection) {
            gui_clear_selection(gui);
            return;
        }
        break;

    case XK_Tab:
        autocomplete_handle_tab(sb);
        screen_buf_scroll_reset(sb);
        gui_redraw(gui);
        return;

    case XK_Return: {
        /* Grab currently typed line */
        char current_line[MAX_CMD_LEN];
        strncpy(current_line, sb->input, sizeof(current_line) - 1);
        current_line[sizeof(current_line) - 1] = '\0';

        /* If in Ctrl+R reverse history search mode */
        if (sb->in_search) {
            screen_buf_input_commit(sb);
            screen_buf_scroll_reset(sb);

            HistorySearchResult res;
            history_search_find(gui->shared, current_line, &res);

            sb->in_search = 0;
            screen_buf_set_prompt(sb, sb->saved_prompt[0] ? sb->saved_prompt : get_default_prompt_str());

            if (res.is_exact && res.count == 1) {
                /* Exact match: replace current line with it */
                screen_buf_input_clear(sb);
                for (int i = 0; res.matches[0][i]; i++) {
                    screen_buf_input_insert(sb, res.matches[0][i]);
                }
            } else if (res.count > 0) {
                /* Several matches: show in scrollback from most recent to oldest */
                char out_buf[4096];
                history_search_format(&res, out_buf, sizeof(out_buf));

                char *line_ptr = out_buf;
                while (*line_ptr) {
                    char *nl = strchr(line_ptr, '\n');
                    if (nl) *nl = '\0';
                    if (*line_ptr != '\0') {
                        screen_buf_add_line(sb, line_ptr);
                    }
                    if (!nl) break;
                    line_ptr = nl + 1;
                }
                screen_buf_input_clear(sb);
            } else {
                /* No match: display "No match for search term in history" and leave command line unchanged */
                screen_buf_add_line(sb, "No match for search term in history");
                screen_buf_input_clear(sb);
                for (int i = 0; sb->saved_input[i]; i++) {
                    screen_buf_input_insert(sb, sb->saved_input[i]);
                }
            }
            break;
        }

        /* ── Command running mode: forward line to worker ── */
        if (gui->tabs[gui->active_tab].command_running) {
            char current_line[MAX_CMD_LEN];
            strncpy(current_line, sb->input, sizeof(current_line) - 1);
            current_line[sizeof(current_line) - 1] = '\0';

            screen_buf_add_line(sb, current_line);
            screen_buf_input_clear(sb);
            screen_buf_scroll_reset(sb);

            int fd = gui->tabs[gui->active_tab].worker.gui_write_fd;
            if (fd >= 0) {
                proto_send(fd, GUI_MSG_LINE, current_line, (uint32_t)strlen(current_line));
            }
            break;
        }

        /* Visually commit the line to scrollback */
        screen_buf_input_commit(sb);
        screen_buf_scroll_reset(sb);
        sb->history_idx = -1;

        /* Multiline accumulation */
        if (!sb->in_multiline) {
            sb->multiline_buf[0] = '\0';
        } else {
            /* If we were already in multiline mode, append newline unless previous line ended with trailing backslash */
            size_t mlen = strlen(sb->multiline_buf);
            size_t end_idx = mlen;
            while (end_idx > 0 && isspace((unsigned char)sb->multiline_buf[end_idx - 1])) {
                end_idx--;
            }
            size_t bslash_count = 0;
            ssize_t bi = (ssize_t)end_idx - 1;
            while (bi >= 0 && sb->multiline_buf[bi] == '\\') {
                bslash_count++;
                bi--;
            }
            if (bslash_count % 2 == 1) {
                /* Trailing backslash line continuation: remove trailing backslash and any spaces after it */
                sb->multiline_buf[end_idx - 1] = '\0';
            } else {
                if (mlen < sizeof(sb->multiline_buf) - 2) {
                    sb->multiline_buf[mlen] = '\n';
                    sb->multiline_buf[mlen + 1] = '\0';
                }
            }
        }

        /* Append current_line as typed to multiline buffer */
        strncat(sb->multiline_buf, current_line,
                sizeof(sb->multiline_buf) - strlen(sb->multiline_buf) - 1);

        /* Test if accumulated input is incomplete */
        if (multiline_is_incomplete(sb->multiline_buf)) {
            sb->in_multiline = 1;
            screen_buf_set_prompt(sb, "> ");
        } else {
            /* Command is complete! */
            sb->in_multiline = 0;
            screen_buf_set_prompt(sb, get_default_prompt_str());

            int fd = gui->tabs[gui->active_tab].worker.gui_write_fd;
            if (fd >= 0 && sb->multiline_buf[0] != '\0') {
                gui->tabs[gui->active_tab].command_running = 1;
                proto_send(fd, GUI_MSG_LINE, sb->multiline_buf,
                           (uint32_t)strlen(sb->multiline_buf));
            }
            sb->multiline_buf[0] = '\0';
        }
        break;
    }

    case XK_BackSpace:
        if (gui->has_selection) gui->has_selection = 0;
        screen_buf_input_delete_back(sb);
        screen_buf_scroll_reset(sb);
        break;

    case XK_Delete:
        if (gui->has_selection) gui->has_selection = 0;
        screen_buf_input_delete_forward(sb);
        break;

    case XK_Left:
        screen_buf_input_move_left(sb);
        break;

    case XK_Right:
        screen_buf_input_move_right(sb);
        break;

    case XK_Home:
        screen_buf_input_home(sb);
        break;

    case XK_End:
        screen_buf_input_end(sb);
        break;

    case XK_Prior:  /* Page Up */
        screen_buf_scroll_up(sb, gui->term_rows > 3 ? gui->term_rows - 3 : 1);
        TERM_LOG("tab[%d]: PageUp, scroll offset=%d", gui->active_tab, sb->scroll_offset);
        break;

    case XK_Next:   /* Page Down */
        screen_buf_scroll_down(sb, gui->term_rows > 3 ? gui->term_rows - 3 : 1);
        TERM_LOG("tab[%d]: PageDown, scroll offset=%d", gui->active_tab, sb->scroll_offset);
        break;

    case XK_Up:
        if (shift) {
            /* Shift+Up scrolls up */
            screen_buf_scroll_up(sb, 3);
            TERM_LOG("tab[%d]: Shift+Up, scroll offset=%d", gui->active_tab, sb->scroll_offset);
        } else if (gui->shared) {
            /* If browsing history while in multiline prompt, reset multiline mode */
            if (sb->in_multiline) {
                sb->in_multiline = 0;
                sb->multiline_buf[0] = '\0';
                screen_buf_set_prompt(sb, get_default_prompt_str());
            }
            /* Up arrow browses history */
            int total = shm_history_count(gui->shared);
            if (total > 0) {
                if (sb->history_idx == -1) {
                    strncpy(sb->saved_input, sb->input, sizeof(sb->saved_input) - 1);
                    sb->saved_input[sizeof(sb->saved_input) - 1] = '\0';
                    sb->history_idx = total - 1;
                } else if (sb->history_idx > 0) {
                    sb->history_idx--;
                }
                const char *cmd_hist = shm_history_get(gui->shared, sb->history_idx);
                if (cmd_hist) {
                    screen_buf_input_clear(sb);
                    for (int i = 0; cmd_hist[i]; i++) {
                        screen_buf_input_insert(sb, cmd_hist[i]);
                    }
                }
                TERM_LOG("tab[%d]: history browse idx=%d", gui->active_tab, sb->history_idx);
            }
        }
        break;

    case XK_Down:
        if (shift) {
            /* Shift+Down scrolls down */
            screen_buf_scroll_down(sb, 3);
            TERM_LOG("tab[%d]: Shift+Down, scroll offset=%d", gui->active_tab, sb->scroll_offset);
        } else if (gui->shared && sb->history_idx != -1) {
            /* If browsing history while in multiline prompt, reset multiline mode */
            if (sb->in_multiline) {
                sb->in_multiline = 0;
                sb->multiline_buf[0] = '\0';
                screen_buf_set_prompt(sb, get_default_prompt_str());
            }
            /* Down arrow browses history forward */
            int total = shm_history_count(gui->shared);
            sb->history_idx++;
            if (sb->history_idx >= total) {
                sb->history_idx = -1;
                screen_buf_input_clear(sb);
                for (int i = 0; sb->saved_input[i]; i++) {
                    screen_buf_input_insert(sb, sb->saved_input[i]);
                }
            } else {
                const char *cmd_hist = shm_history_get(gui->shared, sb->history_idx);
                if (cmd_hist) {
                    screen_buf_input_clear(sb);
                    for (int i = 0; cmd_hist[i]; i++) {
                        screen_buf_input_insert(sb, cmd_hist[i]);
                    }
                }
            }
            TERM_LOG("tab[%d]: history browse forward idx=%d", gui->active_tab, sb->history_idx);
        }
        break;

    default:
        /* Insert printable characters */
        if (len > 0 && (unsigned char)buf[0] >= 32 && !ctrl && !alt && !cmd) {
            if (gui->has_selection) gui->has_selection = 0;
            for (int i = 0; i < len; i++) {
                screen_buf_input_insert(sb, buf[i]);
            }
            screen_buf_scroll_reset(sb);
        }
        break;
    }

    gui_redraw(gui);
}

/* ── Handle mouse button press ──────────────────────────────────────── */
static void handle_button_press(GuiState *gui, XButtonEvent *ev) {
    TERM_LOG("button_press: button=%u at (%d, %d)", ev->button, ev->x, ev->y);

    /* Mouse wheel: Button4=scroll up, Button5=scroll down */
    if (ev->button == Button4 || ev->button == Button5) {
        if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
            ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
            if (ev->button == Button4)
                screen_buf_scroll_up(sb, 3);
            else
                screen_buf_scroll_down(sb, 3);
            TERM_LOG("tab[%d]: mouse wheel scroll offset=%d", gui->active_tab, sb->scroll_offset);
            gui_redraw(gui);
        }
        return;
    }

    int bar_h = gui->tab_bar.bar_height;

    /* Check if click is on the tab bar */
    if (ev->y < bar_h) {
        if (ev->button == Button1) {
            int action = tab_bar_click(&gui->tab_bar, ev->x, ev->y);
            if (action == TAB_ACTION_NEW) {
                TERM_LOG("tab bar '+' clicked: opening new tab");
                gui_open_tab(gui);
                gui_redraw(gui);
            } else if (action >= 0 && action < MAX_TABS && gui->tabs[action].in_use) {
                gui->active_tab = action;
                TERM_LOG("tab switch: clicked tab %d", action + 1);
                gui_redraw(gui);
            }
        } else if (ev->button == Button2 || ev->button == Button3) {
            int action = tab_bar_click(&gui->tab_bar, ev->x, ev->y);
            if (action >= 0 && action < MAX_TABS && gui->tabs[action].in_use) {
                TERM_LOG("tab[%d]: closed via tab click", action);
                gui_close_tab(gui, action);
                gui_redraw(gui);
            }
        }
        return;
    }

    /* Content area clicks */
    if (ev->button == Button1) {
        int x_margin = 4;
        int row = (ev->y - bar_h) / (gui->char_height > 0 ? gui->char_height : 1);
        int col = (ev->x - x_margin) / (gui->char_width > 0 ? gui->char_width : 1);
        if (row < 0) row = 0;
        if (row >= gui->term_rows) row = gui->term_rows - 1;
        if (col < 0) col = 0;
        if (col >= gui->term_cols) col = gui->term_cols - 1;

        gui->is_selecting  = 1;
        gui->has_selection = 0;
        gui->sel_start_row = row;
        gui->sel_start_col = col;
        gui->sel_end_row   = row;
        gui->sel_end_col   = col;
        TERM_LOG("mouse selection started at row=%d col=%d", row, col);
        gui_redraw(gui);
    } else if (ev->button == Button2) {
        TERM_LOG("mouse middle click paste");
        clipboard_request_paste(gui);
    }
}

/* ── Handle mouse drag for text selection ───────────────────────────── */
static void handle_motion_notify(GuiState *gui, XMotionEvent *ev) {
    if (!gui->is_selecting) return;

    int bar_h = gui->tab_bar.bar_height;
    int x_margin = 4;
    int row = (ev->y - bar_h) / (gui->char_height > 0 ? gui->char_height : 1);
    int col = (ev->x - x_margin) / (gui->char_width > 0 ? gui->char_width : 1);
    if (row < 0) row = 0;
    if (row >= gui->term_rows) row = gui->term_rows - 1;
    if (col < 0) col = 0;
    if (col >= gui->term_cols) col = gui->term_cols - 1;

    if (row != gui->sel_end_row || col != gui->sel_end_col) {
        gui->sel_end_row = row;
        gui->sel_end_col = col;
        if (gui->sel_start_row != gui->sel_end_row || gui->sel_start_col != gui->sel_end_col) {
            gui->has_selection = 1;
        }
        gui_redraw(gui);
    }
}

/* ── Handle mouse button release ────────────────────────────────────── */
static void handle_button_release(GuiState *gui, XButtonEvent *ev) {
    TERM_LOG("button_release: button=%u at (%d, %d)", ev->button, ev->x, ev->y);
    if (ev->button == Button1 && gui->is_selecting) {
        gui->is_selecting = 0;
        if (gui->has_selection) {
            gui_copy_selection(gui);
        }
        gui_redraw(gui);
    }
}

/* ── Dispatch a single X11 event ────────────────────────────────────── */
static void handle_x11_event(GuiState *gui, XEvent *ev) {
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0)
            gui_redraw(gui);
        break;

    case KeyPress:
        handle_keypress(gui, &ev->xkey);
        break;

    case ButtonPress:
        handle_button_press(gui, &ev->xbutton);
        break;

    case ButtonRelease:
        handle_button_release(gui, &ev->xbutton);
        break;

    case MotionNotify:
        handle_motion_notify(gui, &ev->xmotion);
        break;

    case SelectionRequest:
        clipboard_handle_selection_request(gui, &ev->xselectionrequest);
        break;

    case SelectionNotify:
        clipboard_handle_selection_notify(gui, &ev->xselection);
        gui_redraw(gui);
        break;

    case ConfigureNotify: {
        int new_w = ev->xconfigure.width;
        int new_h = ev->xconfigure.height;
        if (new_w != gui->win_width || new_h != gui->win_height) {
            gui->win_width  = new_w;
            gui->win_height = new_h;
            recalc_grid(gui);
            recreate_backbuffer(gui);
            gui_redraw(gui);
        }
        break;
    }

    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == gui->wm_delete_msg) {
            TERM_LOG("gui: window close requested");
            gui->running = 0;
        }
        break;

    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  MAIN EVENT LOOP (poll-based, zero busy-wait)
 * ══════════════════════════════════════════════════════════════════════ */

void gui_run(GuiState *gui) {
    int x11_fd = ConnectionNumber(gui->display);
    struct pollfd fds[MAX_TABS + 1];

    gui_redraw(gui);

    while (gui->running) {
        int nfds = 0;

        /* Slot 0: X11 connection */
        fds[nfds].fd     = x11_fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;
        nfds++;

        /* Build a map: fds[nfds] → tab index */
        int fd_to_tab[MAX_TABS + 1];
        for (int i = 0; i < MAX_TABS; i++) {
            if (gui->tabs[i].in_use && gui->tabs[i].worker.gui_read_fd >= 0) {
                fd_to_tab[nfds] = i;
                fds[nfds].fd      = gui->tabs[i].worker.gui_read_fd;
                fds[nfds].events  = POLLIN;
                fds[nfds].revents = 0;
                nfds++;
            }
        }

        /* Block until something is ready — no busy-wait */
        int ret = poll(fds, (nfds_t)nfds, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            TERM_LOG("gui_run: poll error: %s", strerror(errno));
            break;
        }

        /* Process X11 events */
        if (fds[0].revents & POLLIN) {
            while (XPending(gui->display) > 0) {
                XEvent ev;
                XNextEvent(gui->display, &ev);
                handle_x11_event(gui, &ev);
                if (!gui->running) break;
            }
        }

        if (!gui->running) break;

        /* Process tab pipe data */
        for (int f = 1; f < nfds; f++) {
            if (fds[f].revents & (POLLIN | POLLHUP)) {
                int tab_idx = fd_to_tab[f];
                if (gui->tabs[tab_idx].in_use)
                    handle_tab_output(gui, tab_idx);
            }
        }
    }

    TERM_LOG("gui_run: event loop exited");
}
