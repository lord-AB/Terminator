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
 *  RENDERING
 * ══════════════════════════════════════════════════════════════════════ */

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
        int stored    = screen_buf_visible_count(sb);
        int x_margin  = 4;

        int content_rows = gui->term_rows - 1; /* reserve row for input */
        if (content_rows < 1) content_rows = 1;

        /* Cap scroll_offset so it never scrolls past oldest available lines */
        int max_scroll = (stored > content_rows) ? (stored - content_rows) : 0;
        if (sb->scroll_offset > max_scroll) {
            sb->scroll_offset = max_scroll;
        }

        /* Calculate visible scrollback window */
        int end = stored - sb->scroll_offset;
        int start = end > content_rows ? (end - content_rows) : 0;
        int visible_lines = end - start;

        for (int r = 0; r < visible_lines; r++) {
            int idx = start + r;
            const char *line = screen_buf_get_line(sb, idx);
            if (line && line[0]) {
                int y = bar_h + r * gui->char_height + gui->font_ascent;
                XSetForeground(dpy, gc, gui->color_fg);
                int len = (int)strlen(line);
                if (len > gui->term_cols) len = gui->term_cols;
                XDrawString(dpy, buf, gc, x_margin, y, line, len);
            }
        }

        /* ── Draw input line (prompt + input + cursor) ───────────────────── */
        int input_row = (stored < content_rows && sb->scroll_offset == 0)
                        ? stored
                        : content_rows;
        int input_y   = bar_h + input_row * gui->char_height + gui->font_ascent;

        /* Prompt (green) */
        int prompt_len = (int)strlen(sb->prompt);
        if (prompt_len > 0) {
            XSetForeground(dpy, gc, gui->color_prompt);
            int draw_plen = prompt_len > gui->term_cols ? gui->term_cols : prompt_len;
            XDrawString(dpy, buf, gc, x_margin, input_y, sb->prompt, draw_plen);
        }

        /* Input text (white/fg) */
        int input_x = x_margin + prompt_len * gui->char_width;
        if (sb->input_len > 0) {
            XSetForeground(dpy, gc, gui->color_fg);
            int draw_ilen = sb->input_len;
            int max_input_chars = gui->term_cols - prompt_len;
            if (draw_ilen > max_input_chars && max_input_chars > 0)
                draw_ilen = max_input_chars;
            if (draw_ilen > 0)
                XDrawString(dpy, buf, gc, input_x, input_y, sb->input, draw_ilen);
        }

        /* Cursor (block) */
        int cursor_x = input_x + sb->cursor_pos * gui->char_width;
        int cursor_y = bar_h + input_row * gui->char_height;
        XSetForeground(dpy, gc, gui->color_cursor);
        XFillRectangle(dpy, buf, gc, cursor_x, cursor_y,
                       (unsigned)gui->char_width, (unsigned)gui->char_height);
        /* Draw char under cursor in background color (inverse) */
        if (sb->cursor_pos < sb->input_len) {
            XSetForeground(dpy, gc, gui->color_bg);
            XDrawString(dpy, buf, gc, cursor_x, input_y,
                        &sb->input[sb->cursor_pos], 1);
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

    gui_redraw(gui);
}

/* ── Handle keyboard input ──────────────────────────────────────────── */
static void handle_keypress(GuiState *gui, XKeyEvent *ev) {
    char   buf[32];
    KeySym keysym = 0;
    int    len = XLookupString(ev, buf, sizeof(buf) - 1, &keysym, NULL);
    buf[len] = '\0';

    int ctrl  = (ev->state & ControlMask) != 0;
    int shift = (ev->state & ShiftMask) != 0;
    int alt   = (ev->state & Mod1Mask) != 0;
    int cmd   = (ev->state & Mod4Mask) != 0;
    UNUSED(shift);

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
                if (sb->input_len > 0 || sb->in_multiline || !gui->tabs[gui->active_tab].command_running) {
                    char line[MAX_CMD_LEN + 32];
                    snprintf(line, sizeof(line), "%s%s^C", sb->prompt, sb->input);
                    screen_buf_add_line(sb, line);
                    screen_buf_input_clear(sb);
                    sb->in_multiline = 0;
                    sb->multiline_buf[0] = '\0';
                    screen_buf_set_prompt(sb, "user@terminator> ");
                }
                screen_buf_scroll_reset(sb);
                gui_redraw(gui);
            }
        }
        return;
    }

    /* ── Ctrl+T / Cmd+T: new tab ───────────────────────────────────── */
    if ((ctrl || cmd) && (keysym == XK_t || keysym == XK_T)) {
        gui_open_tab(gui);
        gui_redraw(gui);
        return;
    }

    /* ── Alt+1..9: switch tabs ──────────────────────────────────────── */
    if (alt && keysym >= XK_1 && keysym <= XK_9) {
        int target_num = (int)(keysym - XK_1);  /* 0-based */
        int count = 0;
        for (int i = 0; i < MAX_TABS; i++) {
            if (gui->tabs[i].in_use) {
                if (count == target_num) {
                    gui->active_tab = i;
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

    /* ── Configurable Hotkeys (line_editor & clipboard actions) ─────── */
    HotkeyAction action = hotkey_cfg_match(&gui->hotkeys, keysym, ev->state);
    if (action != HOTKEY_ACTION_NONE) {
        if (line_editor_handle_action(gui, sb, action)) {
            gui_redraw(gui);
            return;
        }
    }

    switch (keysym) {
    case XK_Return: {
        /* Grab currently typed line */
        char current_line[MAX_CMD_LEN];
        strncpy(current_line, sb->input, sizeof(current_line) - 1);
        current_line[sizeof(current_line) - 1] = '\0';

        /* Visually commit the line to scrollback */
        screen_buf_input_commit(sb);
        screen_buf_scroll_reset(sb);
        sb->history_idx = -1;

        /* Multiline accumulation */
        if (!sb->in_multiline) {
            sb->multiline_buf[0] = '\0';
        } else {
            /* If we were already in multiline mode, append newline */
            size_t mlen = strlen(sb->multiline_buf);
            if (mlen < sizeof(sb->multiline_buf) - 2) {
                sb->multiline_buf[mlen] = '\n';
                sb->multiline_buf[mlen + 1] = '\0';
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
            screen_buf_set_prompt(sb, "user@terminator> ");

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
        screen_buf_input_delete_back(sb);
        screen_buf_scroll_reset(sb);
        break;

    case XK_Delete:
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
        break;

    case XK_Next:   /* Page Down */
        screen_buf_scroll_down(sb, gui->term_rows > 3 ? gui->term_rows - 3 : 1);
        break;

    case XK_Up:
        if (shift) {
            /* Shift+Up scrolls up */
            screen_buf_scroll_up(sb, 3);
        } else if (gui->shared) {
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
                const char *cmd = shm_history_get(gui->shared, sb->history_idx);
                if (cmd) {
                    screen_buf_input_clear(sb);
                    for (int i = 0; cmd[i]; i++) {
                        screen_buf_input_insert(sb, cmd[i]);
                    }
                }
            }
        }
        break;

    case XK_Down:
        if (shift) {
            /* Shift+Down scrolls down */
            screen_buf_scroll_down(sb, 3);
        } else if (gui->shared && sb->history_idx != -1) {
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
                const char *cmd = shm_history_get(gui->shared, sb->history_idx);
                if (cmd) {
                    screen_buf_input_clear(sb);
                    for (int i = 0; cmd[i]; i++) {
                        screen_buf_input_insert(sb, cmd[i]);
                    }
                }
            }
        }
        break;

    default:
        /* Insert printable characters */
        if (len > 0 && (unsigned char)buf[0] >= 32 && !ctrl && !alt && !cmd) {
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
    /* Mouse wheel: Button4=scroll up, Button5=scroll down */
    if (ev->button == Button4 || ev->button == Button5) {
        if (gui->active_tab >= 0 && gui->tabs[gui->active_tab].in_use) {
            ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
            if (ev->button == Button4)
                screen_buf_scroll_up(sb, 3);
            else
                screen_buf_scroll_down(sb, 3);
            gui_redraw(gui);
        }
        return;
    }

    /* Left click: check tab bar */
    if (ev->button == Button1) {
        int action = tab_bar_click(&gui->tab_bar, ev->x, ev->y);
        if (action == TAB_ACTION_NEW) {
            gui_open_tab(gui);
            gui_redraw(gui);
        } else if (action >= 0 && action < MAX_TABS && gui->tabs[action].in_use) {
            gui->active_tab = action;
            gui_redraw(gui);
        }
        /* TAB_ACTION_NONE — click was outside tab bar or on empty space */
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
