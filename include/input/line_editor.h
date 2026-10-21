/*
 * line_editor.h — Cursor movement, text insertion, and Ctrl+A/E/U/K/W manipulation
 */
#ifndef INPUT_LINE_EDITOR_H
#define INPUT_LINE_EDITOR_H

#include "common.h"
#include "gui/screen_buf.h"
#include "input/hotkey_cfg.h"

struct GuiState; /* Forward declaration */

/* Cursor movement operations */
void line_editor_move_left(ScreenBuf *sb);
void line_editor_move_right(ScreenBuf *sb);
void line_editor_move_home(ScreenBuf *sb);
void line_editor_move_end(ScreenBuf *sb);

/* Editing operations */
void line_editor_delete_to_start(ScreenBuf *sb);
void line_editor_delete_to_end(ScreenBuf *sb);
void line_editor_delete_prev_word(ScreenBuf *sb);
void line_editor_clear_screen(ScreenBuf *sb);

/*
 * Dispatches an action onto the active tab screen buffer and GUI.
 * Returns 1 if the action was handled, 0 otherwise.
 */
int line_editor_handle_action(struct GuiState *gui, ScreenBuf *sb, HotkeyAction action);

#endif /* INPUT_LINE_EDITOR_H */
