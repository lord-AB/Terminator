/*
 * line_editor.c — Implementation of line editing operations and action dispatching
 */
#include "input/line_editor.h"
#include "gui/x11_window.h"
#include "gui/clipboard.h"
#include "ipc/shm_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void line_editor_move_left(ScreenBuf *sb) {
    if (!sb) return;
    screen_buf_input_move_left(sb);
}

void line_editor_move_right(ScreenBuf *sb) {
    if (!sb) return;
    screen_buf_input_move_right(sb);
}

void line_editor_move_home(ScreenBuf *sb) {
    if (!sb) return;
    screen_buf_input_home(sb);
}

void line_editor_move_end(ScreenBuf *sb) {
    if (!sb) return;
    screen_buf_input_end(sb);
}

void line_editor_delete_to_start(ScreenBuf *sb) {
    if (!sb || sb->cursor_pos <= 0 || sb->input_len <= 0) return;

    int to_remove = sb->cursor_pos;
    memmove(&sb->input[0],
            &sb->input[sb->cursor_pos],
            sb->input_len - sb->cursor_pos + 1);

    sb->input_len -= to_remove;
    sb->cursor_pos = 0;
}

void line_editor_delete_to_end(ScreenBuf *sb) {
    if (!sb || sb->cursor_pos >= sb->input_len) return;

    sb->input[sb->cursor_pos] = '\0';
    sb->input_len = sb->cursor_pos;
}

void line_editor_delete_prev_word(ScreenBuf *sb) {
    if (!sb || sb->cursor_pos <= 0 || sb->input_len <= 0) return;

    int cur = sb->cursor_pos;
    /* 1. Skip whitespace immediately preceding cursor */
    while (cur > 0 && isspace((unsigned char)sb->input[cur - 1])) {
        cur--;
    }
    /* 2. Skip non-whitespace word characters */
    while (cur > 0 && !isspace((unsigned char)sb->input[cur - 1])) {
        cur--;
    }

    int start_word = cur;
    int chars_to_remove = sb->cursor_pos - start_word;
    if (chars_to_remove <= 0) return;

    memmove(&sb->input[start_word],
            &sb->input[sb->cursor_pos],
            sb->input_len - sb->cursor_pos + 1);

    sb->input_len -= chars_to_remove;
    sb->cursor_pos = start_word;
}

void line_editor_clear_screen(ScreenBuf *sb) {
    if (!sb) return;
    screen_buf_clear(sb);
}

int line_editor_handle_action(GuiState *gui, ScreenBuf *sb, HotkeyAction action) {
    if (!sb) return 0;

    switch (action) {
    case HOTKEY_ACTION_MOVE_LEFT:
        line_editor_move_left(sb);
        return 1;

    case HOTKEY_ACTION_MOVE_RIGHT:
        line_editor_move_right(sb);
        return 1;

    case HOTKEY_ACTION_MOVE_HOME:
        line_editor_move_home(sb);
        return 1;

    case HOTKEY_ACTION_MOVE_END:
        line_editor_move_end(sb);
        return 1;

    case HOTKEY_ACTION_DELETE_TO_START:
        line_editor_delete_to_start(sb);
        return 1;

    case HOTKEY_ACTION_DELETE_TO_END:
        line_editor_delete_to_end(sb);
        return 1;

    case HOTKEY_ACTION_DELETE_PREV_WORD:
        line_editor_delete_prev_word(sb);
        return 1;

    case HOTKEY_ACTION_CLEAR_SCREEN:
        line_editor_clear_screen(sb);
        return 1;

    case HOTKEY_ACTION_EXIT_OR_EOF:
        if (sb->input_len == 0) {
            if (gui && gui->active_tab >= 0) {
                gui_close_tab(gui, gui->active_tab);
            }
        } else {
            screen_buf_input_delete_forward(sb);
        }
        return 1;

    case HOTKEY_ACTION_COPY:
        if (gui) {
            /* Copy entire line if no active selection */
            clipboard_copy(gui, sb->input, sb->input_len);
        }
        return 1;

    case HOTKEY_ACTION_PASTE:
        if (gui) {
            clipboard_request_paste(gui);
        }
        return 1;

    case HOTKEY_ACTION_HISTORY_PREV:
        if (gui && gui->shared) {
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
        return 1;

    case HOTKEY_ACTION_HISTORY_NEXT:
        if (gui && gui->shared && sb->history_idx != -1) {
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
        return 1;

    case HOTKEY_ACTION_SEARCH_HISTORY:
        if (sb) {
            sb->in_search = 1;
            strncpy(sb->saved_prompt, sb->prompt, sizeof(sb->saved_prompt) - 1);
            sb->saved_prompt[sizeof(sb->saved_prompt) - 1] = '\0';
            screen_buf_set_prompt(sb, "Enter search term: ");
            screen_buf_input_clear(sb);
        }
        return 1;

    default:
        break;
    }

    return 0;
}
