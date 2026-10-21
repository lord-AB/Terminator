/*
 * test_hotkeys.c — Unit tests for Task 7 hotkeys, line editor, and custom keybindings
 */
#include "input/hotkey_cfg.h"
#include "input/line_editor.h"
#include "gui/screen_buf.h"
#include "gui/clipboard.h"
#include "gui/x11_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS() printf("  ✓ %s passed\n", __func__)

/* Stub for headless unit testing */
void gui_close_tab(GuiState *gui, int idx) {
    (void)gui;
    (void)idx;
}

static void test_default_hotkey_bindings(void) {
    HotkeyConfig cfg;
    hotkey_cfg_init_defaults(&cfg);

    /* Test all 13 keys from the design.md table */
    assert(hotkey_cfg_match(&cfg, XK_Left, 0) == HOTKEY_ACTION_MOVE_LEFT);
    assert(hotkey_cfg_match(&cfg, XK_Right, 0) == HOTKEY_ACTION_MOVE_RIGHT);
    assert(hotkey_cfg_match(&cfg, XK_a, ControlMask) == HOTKEY_ACTION_MOVE_HOME);
    assert(hotkey_cfg_match(&cfg, XK_A, ControlMask) == HOTKEY_ACTION_MOVE_HOME);
    assert(hotkey_cfg_match(&cfg, XK_e, ControlMask) == HOTKEY_ACTION_MOVE_END);
    assert(hotkey_cfg_match(&cfg, XK_E, ControlMask) == HOTKEY_ACTION_MOVE_END);
    assert(hotkey_cfg_match(&cfg, XK_Up, 0) == HOTKEY_ACTION_HISTORY_PREV);
    assert(hotkey_cfg_match(&cfg, XK_Down, 0) == HOTKEY_ACTION_HISTORY_NEXT);
    assert(hotkey_cfg_match(&cfg, XK_u, ControlMask) == HOTKEY_ACTION_DELETE_TO_START);
    assert(hotkey_cfg_match(&cfg, XK_U, ControlMask) == HOTKEY_ACTION_DELETE_TO_START);
    assert(hotkey_cfg_match(&cfg, XK_k, ControlMask) == HOTKEY_ACTION_DELETE_TO_END);
    assert(hotkey_cfg_match(&cfg, XK_K, ControlMask) == HOTKEY_ACTION_DELETE_TO_END);
    assert(hotkey_cfg_match(&cfg, XK_w, ControlMask) == HOTKEY_ACTION_DELETE_PREV_WORD);
    assert(hotkey_cfg_match(&cfg, XK_W, ControlMask) == HOTKEY_ACTION_DELETE_PREV_WORD);
    assert(hotkey_cfg_match(&cfg, XK_l, ControlMask) == HOTKEY_ACTION_CLEAR_SCREEN);
    assert(hotkey_cfg_match(&cfg, XK_L, ControlMask) == HOTKEY_ACTION_CLEAR_SCREEN);
    assert(hotkey_cfg_match(&cfg, XK_d, ControlMask) == HOTKEY_ACTION_EXIT_OR_EOF);
    assert(hotkey_cfg_match(&cfg, XK_D, ControlMask) == HOTKEY_ACTION_EXIT_OR_EOF);
    assert(hotkey_cfg_match(&cfg, XK_c, ControlMask | ShiftMask) == HOTKEY_ACTION_COPY);
    assert(hotkey_cfg_match(&cfg, XK_C, ControlMask | ShiftMask) == HOTKEY_ACTION_COPY);
    assert(hotkey_cfg_match(&cfg, XK_v, ControlMask | ShiftMask) == HOTKEY_ACTION_PASTE);
    assert(hotkey_cfg_match(&cfg, XK_V, ControlMask | ShiftMask) == HOTKEY_ACTION_PASTE);

    /* Test macOS Cmd (Mod4Mask) copy and paste fallback */
    assert(hotkey_cfg_match(&cfg, XK_c, Mod4Mask) == HOTKEY_ACTION_COPY);
    assert(hotkey_cfg_match(&cfg, XK_C, Mod4Mask) == HOTKEY_ACTION_COPY);
    assert(hotkey_cfg_match(&cfg, XK_v, Mod4Mask) == HOTKEY_ACTION_PASTE);
    assert(hotkey_cfg_match(&cfg, XK_V, Mod4Mask) == HOTKEY_ACTION_PASTE);

    TEST_PASS();
}

static void test_line_editor_cursor_and_editing(void) {
    ScreenBuf *sb = screen_buf_create();
    assert(sb != NULL);

    /* Setup initial text: "echo hello world" */
    const char *text = "echo hello world";
    for (int i = 0; text[i]; i++) {
        screen_buf_input_insert(sb, text[i]);
    }
    assert(strcmp(sb->input, "echo hello world") == 0);
    assert(sb->cursor_pos == 16);
    assert(sb->input_len == 16);

    /* 1. Move home (Ctrl+A) */
    line_editor_move_home(sb);
    assert(sb->cursor_pos == 0);

    /* 2. Move right 5 times */
    for (int i = 0; i < 5; i++) line_editor_move_right(sb);
    assert(sb->cursor_pos == 5);

    /* 3. Move left 2 times */
    line_editor_move_left(sb);
    line_editor_move_left(sb);
    assert(sb->cursor_pos == 3);

    /* 4. Move end (Ctrl+E) */
    line_editor_move_end(sb);
    assert(sb->cursor_pos == 16);

    /* 5. Delete previous word (Ctrl+W) from end: deletes "world" */
    line_editor_delete_prev_word(sb);
    assert(strcmp(sb->input, "echo hello ") == 0);
    assert(sb->cursor_pos == 11);
    assert(sb->input_len == 11);

    /* 6. Delete previous word again: deletes "hello" */
    line_editor_delete_prev_word(sb);
    assert(strcmp(sb->input, "echo ") == 0);
    assert(sb->cursor_pos == 5);
    assert(sb->input_len == 5);

    /* Reset to "one two three four" */
    screen_buf_input_clear(sb);
    const char *t2 = "one two three four";
    for (int i = 0; t2[i]; i++) screen_buf_input_insert(sb, t2[i]);
    /* Move cursor to index 7 ("one two| three four") */
    sb->cursor_pos = 7;

    /* 7. Delete to start (Ctrl+U): deletes "one two" */
    line_editor_delete_to_start(sb);
    assert(strcmp(sb->input, " three four") == 0);
    assert(sb->cursor_pos == 0);
    assert(sb->input_len == 11);

    /* 8. Move to index 6 (" three| four"), delete to end (Ctrl+K) */
    sb->cursor_pos = 6;
    line_editor_delete_to_end(sb);
    assert(strcmp(sb->input, " three") == 0);
    assert(sb->cursor_pos == 6);
    assert(sb->input_len == 6);

    /* 9. Clear screen (Ctrl+L) */
    screen_buf_add_line(sb, "Line 1");
    screen_buf_add_line(sb, "Line 2");
    assert(screen_buf_visible_count(sb) == 2);
    line_editor_clear_screen(sb);
    assert(screen_buf_visible_count(sb) == 0);

    screen_buf_destroy(sb);
    TEST_PASS();
}

static void test_config_remapping_and_persistence(void) {
    const char *tmp_rc = "/tmp/test_terminatorrc_custom";
    FILE *fp = fopen(tmp_rc, "w");
    assert(fp != NULL);
    fprintf(fp, "# Custom Keybindings\n");
    fprintf(fp, "move_home = Ctrl+H\n");
    fprintf(fp, "move_end = Ctrl+G\n");
    fprintf(fp, "delete_prev_word = Alt+BackSpace\n");
    fprintf(fp, "copy = Cmd+C\n");
    fprintf(fp, "paste = Super+V\n");
    fclose(fp);

    HotkeyConfig cfg;
    char err_msg[512] = {0};
    int errs = hotkey_cfg_load(&cfg, tmp_rc, err_msg, sizeof(err_msg));
    assert(errs == 0);

    /* Verify remapped keys take effect */
    assert(hotkey_cfg_match(&cfg, XK_h, ControlMask) == HOTKEY_ACTION_MOVE_HOME);
    assert(hotkey_cfg_match(&cfg, XK_g, ControlMask) == HOTKEY_ACTION_MOVE_END);
    assert(hotkey_cfg_match(&cfg, XK_BackSpace, Mod1Mask) == HOTKEY_ACTION_DELETE_PREV_WORD);
    assert(hotkey_cfg_match(&cfg, XK_c, Mod4Mask) == HOTKEY_ACTION_COPY);
    assert(hotkey_cfg_match(&cfg, XK_v, Mod4Mask) == HOTKEY_ACTION_PASTE);

    /* Old Ctrl+A is no longer move_home */
    assert(hotkey_cfg_match(&cfg, XK_a, ControlMask) == HOTKEY_ACTION_NONE);

    /* Unmodified bindings retain their defaults */
    assert(hotkey_cfg_match(&cfg, XK_l, ControlMask) == HOTKEY_ACTION_CLEAR_SCREEN);
    assert(hotkey_cfg_match(&cfg, XK_d, ControlMask) == HOTKEY_ACTION_EXIT_OR_EOF);

    unlink(tmp_rc);
    TEST_PASS();
}

static void test_invalid_and_conflicting_config(void) {
    const char *tmp_rc = "/tmp/test_terminatorrc_invalid";
    FILE *fp = fopen(tmp_rc, "w");
    assert(fp != NULL);
    /* 1. Invalid key name */
    fprintf(fp, "move_home = NonExistentKey123\n");
    /* 2. Conflicting key: Ctrl+W is already delete_prev_word */
    fprintf(fp, "clear_screen = Ctrl+W\n");
    /* 3. Unknown action */
    fprintf(fp, "unknown_action_xyz = Ctrl+J\n");
    fclose(fp);

    HotkeyConfig cfg;
    char err_msg[1024] = {0};
    int errs = hotkey_cfg_load(&cfg, tmp_rc, err_msg, sizeof(err_msg));
    assert(errs == 3);
    assert(strlen(err_msg) > 0);

    /* Verify error messages mention the issues */
    assert(strstr(err_msg, "invalid key") != NULL);
    assert(strstr(err_msg, "conflict") != NULL);
    assert(strstr(err_msg, "unknown action") != NULL);

    /* Verify fallbacks: move_home falls back to Ctrl+A, clear_screen falls back to Ctrl+L */
    assert(hotkey_cfg_match(&cfg, XK_a, ControlMask) == HOTKEY_ACTION_MOVE_HOME);
    assert(hotkey_cfg_match(&cfg, XK_l, ControlMask) == HOTKEY_ACTION_CLEAR_SCREEN);
    assert(hotkey_cfg_match(&cfg, XK_w, ControlMask) == HOTKEY_ACTION_DELETE_PREV_WORD);

    unlink(tmp_rc);
    TEST_PASS();
}

static void test_clipboard_operations(void) {
    GuiState gui;
    memset(&gui, 0, sizeof(GuiState));
    clipboard_init(&gui);

    /* Setup a tab and screen buffer */
    gui.tabs[0].in_use = 1;
    gui.tabs[0].screen = screen_buf_create();
    gui.active_tab = 0;

    /* Copy "echo Terminator Clipboard!" */
    const char *src = "echo Terminator Clipboard!";
    clipboard_copy(&gui, src, strlen(src));
    assert(strcmp(gui.clipboard.text, src) == 0);
    assert(gui.clipboard.len == strlen(src));

    /* Request paste into active tab */
    clipboard_request_paste(&gui);
    assert(strcmp(gui.tabs[0].screen->input, src) == 0);
    assert(gui.tabs[0].screen->cursor_pos == (int)strlen(src));

    screen_buf_destroy(gui.tabs[0].screen);
    TEST_PASS();
}

int main(void) {
    printf("=== Running Task 7 Hotkeys & Keybindings Unit Tests ===\n");
    test_default_hotkey_bindings();
    test_line_editor_cursor_and_editing();
    test_config_remapping_and_persistence();
    test_invalid_and_conflicting_config();
    test_clipboard_operations();
    printf("✓ All Hotkeys & Keybindings Unit Tests Passed Successfully!\n");
    return 0;
}
