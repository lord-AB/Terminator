/*
 * test_gui_buffer.c — Unit tests for ScreenBuf and TabBar hit testing
 *
 * Verifies screen buffer ring buffer wrapping, line retrieval, editing,
 * scrolling, clearing, and tab bar hit testing without requiring X11 display.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "common.h"
#include "gui/screen_buf.h"
#include "gui/tab_bar.h"

static void test_screen_buf_basic(void) {
    printf("Testing screen_buf_create and defaults... ");
    ScreenBuf *sb = screen_buf_create();
    assert(sb != NULL);
    assert(strcmp(sb->prompt, "user@terminator> ") == 0);
    assert(sb->count == 0);
    assert(sb->total == 0);
    assert(sb->scroll_offset == 0);
    assert(sb->input_len == 0);
    assert(sb->cursor_pos == 0);
    assert(strlen(sb->cwd) > 0);
    printf("PASS\n");

    /* Add lines */
    printf("Testing screen_buf_add_line and get_line... ");
    screen_buf_add_line(sb, "Line 1");
    screen_buf_add_line(sb, "Line 2");
    screen_buf_add_line(sb, "Line 3");
    assert(screen_buf_visible_count(sb) == 3);
    assert(strcmp(screen_buf_get_line(sb, 0), "Line 1") == 0);
    assert(strcmp(screen_buf_get_line(sb, 1), "Line 2") == 0);
    assert(strcmp(screen_buf_get_line(sb, 2), "Line 3") == 0);
    assert(screen_buf_get_line(sb, 3) == NULL);
    assert(screen_buf_get_line(sb, -1) == NULL);
    printf("PASS\n");

    /* Add text with multiple newlines */
    printf("Testing screen_buf_add_text multi-line... ");
    screen_buf_add_text(sb, "Line 4\nLine 5\nLine 6");
    assert(screen_buf_visible_count(sb) == 6);
    assert(strcmp(screen_buf_get_line(sb, 3), "Line 4") == 0);
    assert(strcmp(screen_buf_get_line(sb, 4), "Line 5") == 0);
    assert(strcmp(screen_buf_get_line(sb, 5), "Line 6") == 0);
    printf("PASS\n");

    /* Ring buffer wrap */
    printf("Testing ring buffer wrap beyond SCROLLBACK_MAX (%d)... ", SCROLLBACK_MAX);
    for (int i = 7; i <= SCROLLBACK_MAX + 100; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Numbered Line %d", i);
        screen_buf_add_line(sb, buf);
    }
    assert(screen_buf_visible_count(sb) == SCROLLBACK_MAX);
    /* Oldest line should now be Line 101 (2100 total - 2000 capacity + 1) */
    const char *oldest = screen_buf_get_line(sb, 0);
    assert(oldest != NULL);
    assert(strcmp(oldest, "Numbered Line 101") == 0);
    /* Newest line should be Line 2100 */
    const char *newest = screen_buf_get_line(sb, SCROLLBACK_MAX - 1);
    assert(newest != NULL);
    assert(strcmp(newest, "Numbered Line 2100") == 0);
    printf("PASS\n");

    /* Scroll test */
    printf("Testing scrolling up/down/reset... ");
    screen_buf_scroll_up(sb, 10);
    assert(sb->scroll_offset == 10);
    screen_buf_scroll_up(sb, SCROLLBACK_MAX * 2); /* overflow cap */
    assert(sb->scroll_offset == SCROLLBACK_MAX - 1);
    screen_buf_scroll_down(sb, 50);
    assert(sb->scroll_offset == SCROLLBACK_MAX - 51);
    screen_buf_scroll_reset(sb);
    assert(sb->scroll_offset == 0);
    printf("PASS\n");

    /* Clear screen (Ctrl+L) */
    printf("Testing screen_buf_clear... ");
    screen_buf_clear(sb);
    assert(screen_buf_visible_count(sb) == 0);
    assert(sb->scroll_offset == 0);
    printf("PASS\n");

    /* Line editing */
    printf("Testing line editing (insert, backspace, delete, cursor)... ");
    screen_buf_input_insert(sb, 'a');
    screen_buf_input_insert(sb, 'c');
    screen_buf_input_move_left(sb);
    assert(sb->cursor_pos == 1);
    screen_buf_input_insert(sb, 'b'); /* insert between a and c -> "abc" */
    assert(strcmp(sb->input, "abc") == 0);
    assert(sb->input_len == 3);
    assert(sb->cursor_pos == 2);

    /* Move home and delete forward */
    screen_buf_input_home(sb);
    assert(sb->cursor_pos == 0);
    screen_buf_input_delete_forward(sb); /* deletes 'a' -> "bc" */
    assert(strcmp(sb->input, "bc") == 0);

    /* Move end and backspace */
    screen_buf_input_end(sb);
    assert(sb->cursor_pos == 2);
    screen_buf_input_delete_back(sb); /* deletes 'c' -> "b" */
    assert(strcmp(sb->input, "b") == 0);

    /* Commit input */
    screen_buf_input_commit(sb);
    assert(sb->input_len == 0);
    assert(screen_buf_visible_count(sb) == 1);
    assert(strstr(screen_buf_get_line(sb, 0), "b") != NULL);
    printf("PASS\n");

    screen_buf_destroy(sb);
}

static void test_tab_bar_hit_test(void) {
    printf("Testing tab_bar hit testing... ");
    TabBarState tbs;
    tab_bar_init(&tbs, 15);
    assert(tbs.bar_height == 25);
    assert(tbs.count == 0);

    /* Simulate 2 tabs and a [+] button */
    tbs.rects[0] = (TabRect){ .x = 2, .y = 1, .w = 80, .h = 23, .action = 0 };
    tbs.rects[1] = (TabRect){ .x = 84, .y = 1, .w = 80, .h = 23, .action = 1 };
    tbs.rects[2] = (TabRect){ .x = 166, .y = 1, .w = 30, .h = 23, .action = TAB_ACTION_NEW };
    tbs.count = 3;

    /* Hit tab 0 */
    assert(tab_bar_click(&tbs, 10, 10) == 0);
    /* Hit tab 1 */
    assert(tab_bar_click(&tbs, 100, 10) == 1);
    /* Hit new tab [+] */
    assert(tab_bar_click(&tbs, 170, 10) == TAB_ACTION_NEW);
    /* Miss: below bar height */
    assert(tab_bar_click(&tbs, 10, 30) == TAB_ACTION_NONE);
    /* Miss: between tabs */
    assert(tab_bar_click(&tbs, 83, 10) == TAB_ACTION_NONE);
    /* Miss: beyond last rect */
    assert(tab_bar_click(&tbs, 500, 10) == TAB_ACTION_NONE);
    printf("PASS\n");
}

int main(void) {
    printf("\n=== Running GUI Buffer & TabBar Unit Tests ===\n");
    test_screen_buf_basic();
    test_tab_bar_hit_test();
    printf("=== All GUI Unit Tests Passed! ===\n\n");
    return 0;
}
