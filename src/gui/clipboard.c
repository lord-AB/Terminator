/*
 * clipboard.c — Implementation of X11 selections copy and paste
 */
#include "gui/clipboard.h"
#include "gui/x11_window.h"
#include "gui/screen_buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>

void clipboard_init(GuiState *gui) {
    if (!gui) return;

    memset(&gui->clipboard, 0, sizeof(ClipboardState));

    if (gui->display) {
        gui->clipboard.atom_clipboard = XInternAtom(gui->display, "CLIPBOARD", False);
        gui->clipboard.atom_utf8      = XInternAtom(gui->display, "UTF8_STRING", False);
        gui->clipboard.atom_targets   = XInternAtom(gui->display, "TARGETS", False);
        gui->clipboard.atom_property  = XInternAtom(gui->display, "TERMINATOR_SELECTION", False);
    }
}

void clipboard_copy(GuiState *gui, const char *text, size_t len) {
    if (!gui || !text || len == 0) return;

    if (len >= sizeof(gui->clipboard.text)) {
        len = sizeof(gui->clipboard.text) - 1;
    }

    memcpy(gui->clipboard.text, text, len);
    gui->clipboard.text[len] = '\0';
    gui->clipboard.len = len;

    if (gui->display && gui->window) {
        XSetSelectionOwner(gui->display, gui->clipboard.atom_clipboard, gui->window, CurrentTime);
        XSetSelectionOwner(gui->display, XA_PRIMARY, gui->window, CurrentTime);
    }
}

void clipboard_request_paste(GuiState *gui) {
    if (!gui) return;

    if (gui->display && gui->window) {
        /* Request UTF8_STRING from CLIPBOARD; if no owner, will report None */
        XConvertSelection(gui->display,
                          gui->clipboard.atom_clipboard,
                          gui->clipboard.atom_utf8,
                          gui->clipboard.atom_property,
                          gui->window,
                          CurrentTime);
    } else {
        /* Fallback for headless testing or in-process paste */
        if (gui->clipboard.len > 0) {
            clipboard_paste_text(gui, gui->clipboard.text, gui->clipboard.len);
        }
    }
}

void clipboard_handle_selection_request(GuiState *gui, XSelectionRequestEvent *req) {
    if (!gui || !req) return;

    XSelectionEvent notify;
    memset(&notify, 0, sizeof(notify));
    notify.type = SelectionNotify;
    notify.display = req->display;
    notify.requestor = req->requestor;
    notify.selection = req->selection;
    notify.target = req->target;
    notify.time = req->time;
    notify.property = req->property;

    if (req->target == gui->clipboard.atom_targets) {
        /* Provide list of supported targets */
        Atom targets[2];
        targets[0] = gui->clipboard.atom_utf8;
        targets[1] = XA_STRING;
        XChangeProperty(req->display, req->requestor, req->property,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)targets, 2);
    } else if (req->target == gui->clipboard.atom_utf8 || req->target == XA_STRING) {
        /* Provide actual clipboard text */
        XChangeProperty(req->display, req->requestor, req->property,
                        req->target, 8, PropModeReplace,
                        (unsigned char *)gui->clipboard.text,
                        (int)gui->clipboard.len);
    } else {
        /* Unsupported target */
        notify.property = None;
    }

    XSendEvent(req->display, req->requestor, False, 0, (XEvent *)&notify);
    XFlush(req->display);
}

void clipboard_handle_selection_notify(GuiState *gui, XSelectionEvent *ev) {
    if (!gui || !ev) return;

    if (ev->property != None) {
        Atom actual_type;
        int actual_format;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop_data = NULL;

        int status = XGetWindowProperty(gui->display, gui->window, ev->property,
                                        0, MAX_CMD_LEN / 4, False, AnyPropertyType,
                                        &actual_type, &actual_format,
                                        &nitems, &bytes_after, &prop_data);
        if (status == Success && prop_data && nitems > 0) {
            clipboard_paste_text(gui, (const char *)prop_data, nitems);
            XFree(prop_data);
            XDeleteProperty(gui->display, gui->window, ev->property);
        }
    } else {
        /* If external CLIPBOARD was empty, fall back to internal buffer */
        if (gui->clipboard.len > 0) {
            clipboard_paste_text(gui, gui->clipboard.text, gui->clipboard.len);
        }
    }
}

void clipboard_paste_text(GuiState *gui, const char *text, size_t len) {
    if (!gui || !text || len == 0) return;
    if (gui->active_tab < 0 || !gui->tabs[gui->active_tab].in_use) return;

    ScreenBuf *sb = gui->tabs[gui->active_tab].screen;
    if (!sb) return;

    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            /* Paste newline */
            screen_buf_input_insert(sb, ' ');
        } else if ((unsigned char)ch >= 32 || ch == '\t') {
            screen_buf_input_insert(sb, ch);
        }
    }
    screen_buf_scroll_reset(sb);
}
