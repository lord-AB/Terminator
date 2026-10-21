/*
 * clipboard.h — X11 Selection handling (Ctrl+Shift+C / Ctrl+Shift+V)
 */
#ifndef GUI_CLIPBOARD_H
#define GUI_CLIPBOARD_H

#include "common.h"
#include <X11/Xlib.h>
#include <stddef.h>

struct GuiState; /* Forward declaration */

typedef struct {
    char   text[MAX_CMD_LEN];
    size_t len;
    Atom   atom_clipboard;
    Atom   atom_utf8;
    Atom   atom_targets;
    Atom   atom_property;
} ClipboardState;

/* Initialize clipboard atoms and internal state */
void clipboard_init(struct GuiState *gui);

/* Copy text to internal clipboard and claim ownership of XA_CLIPBOARD */
void clipboard_copy(struct GuiState *gui, const char *text, size_t len);

/* Request clipboard contents via XConvertSelection */
void clipboard_request_paste(struct GuiState *gui);

/* Handle X11 SelectionRequest event */
void clipboard_handle_selection_request(struct GuiState *gui, XSelectionRequestEvent *req);

/* Handle X11 SelectionNotify event */
void clipboard_handle_selection_notify(struct GuiState *gui, XSelectionEvent *ev);

/* Insert pasted text at the cursor in active tab */
void clipboard_paste_text(struct GuiState *gui, const char *text, size_t len);

#endif /* GUI_CLIPBOARD_H */
