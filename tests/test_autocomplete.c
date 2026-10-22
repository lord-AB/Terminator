/*
 * test_autocomplete.c — Unit tests for file-name auto-completion (Stage 10 / Task 10)
 */
#include "input/autocomplete.h"
#include "gui/screen_buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_PASS() printf("  ✓ %s passed\n", __func__)

static const char *TEST_DIR = "/tmp/test_autocomplete_sandbox";

static void setup_test_files(void) {
    mkdir(TEST_DIR, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/abc.txt", TEST_DIR);
    FILE *f1 = fopen(path, "w");
    if (f1) { fprintf(f1, "abc\n"); fclose(f1); }

    snprintf(path, sizeof(path), "%s/abcd.txt", TEST_DIR);
    FILE *f2 = fopen(path, "w");
    if (f2) { fprintf(f2, "abcd\n"); fclose(f2); }

    snprintf(path, sizeof(path), "%s/def.txt", TEST_DIR);
    FILE *f3 = fopen(path, "w");
    if (f3) { fprintf(f3, "def\n"); fclose(f3); }
}

static void cleanup_test_files(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/abc.txt", TEST_DIR);
    unlink(path);
    snprintf(path, sizeof(path), "%s/abcd.txt", TEST_DIR);
    unlink(path);
    snprintf(path, sizeof(path), "%s/def.txt", TEST_DIR);
    unlink(path);
    rmdir(TEST_DIR);
}

static void test_word_boundary_detection(void) {
    int start = 0, len = 0;

    /* Single word */
    assert(autocomplete_find_word("myprog", 6, &start, &len) == 0);
    assert(start == 0 && len == 6);

    /* Word after space */
    assert(autocomplete_find_word("./myprog de", 11, &start, &len) == 0);
    assert(start == 9 && len == 2);

    /* Word after space with trailing text */
    assert(autocomplete_find_word("./myprog de --flag", 11, &start, &len) == 0);
    assert(start == 9 && len == 2);

    /* Word after redirection operator '<' */
    assert(autocomplete_find_word("sort <infile", 12, &start, &len) == 0);
    assert(start == 6 && len == 6);

    /* Word after pipe '|' */
    assert(autocomplete_find_word("ls |wc", 6, &start, &len) == 0);
    assert(start == 4 && len == 2);

    /* Empty line */
    assert(autocomplete_find_word("", 0, &start, &len) == 0);
    assert(start == 0 && len == 0);

    TEST_PASS();
}

static void test_directory_scan_and_lcp(void) {
    AutocompleteState state;

    /* Scan for "de" -> 1 match: def.txt */
    int count = autocomplete_scan(TEST_DIR, "de", &state);
    assert(count == 1);
    assert(strcmp(state.matches[0], "def.txt") == 0);

    /* Scan for "abc" -> 2 matches: abc.txt, abcd.txt */
    count = autocomplete_scan(TEST_DIR, "abc", &state);
    assert(count == 2);
    assert(strcmp(state.matches[0], "abc.txt") == 0);
    assert(strcmp(state.matches[1], "abcd.txt") == 0);

    /* Compute LCP of "abc.txt" and "abcd.txt" */
    char lcp[64];
    int lcp_len = autocomplete_compute_lcp(&state, lcp, sizeof(lcp));
    assert(lcp_len == 3);
    assert(strcmp(lcp, "abc") == 0);

    /* Scan for "xyz" -> 0 matches */
    count = autocomplete_scan(TEST_DIR, "xyz", &state);
    assert(count == 0);

    TEST_PASS();
}

static void test_exact_single_match_completion(void) {
    ScreenBuf *sb = screen_buf_create();
    assert(sb != NULL);
    screen_buf_set_cwd(sb, TEST_DIR);

    /* Type: "./myprog de" */
    const char *typed = "./myprog de";
    for (int i = 0; typed[i]; i++) {
        screen_buf_input_insert(sb, typed[i]);
    }
    assert(strcmp(sb->input, "./myprog de") == 0);
    assert(sb->cursor_pos == 11);

    /* Press Tab -> should complete to "./myprog def.txt" */
    int res = autocomplete_handle_tab(sb);
    assert(res == 1);
    assert(strcmp(sb->input, "./myprog def.txt") == 0);
    assert(sb->cursor_pos == 16);
    assert(sb->autocomplete.in_select_mode == 0);

    screen_buf_destroy(sb);
    TEST_PASS();
}

static void test_ambiguous_match_and_numbered_menu(void) {
    ScreenBuf *sb = screen_buf_create();
    assert(sb != NULL);
    screen_buf_set_cwd(sb, TEST_DIR);

    /* Type: "./myprog def.txt abc" */
    const char *typed = "./myprog def.txt abc";
    for (int i = 0; typed[i]; i++) {
        screen_buf_input_insert(sb, typed[i]);
    }
    assert(strcmp(sb->input, "./myprog def.txt abc") == 0);
    assert(sb->cursor_pos == 20);

    /* Press Tab -> ambiguous: abc.txt and abcd.txt */
    int res = autocomplete_handle_tab(sb);
    assert(res == 1);
    assert(sb->autocomplete.in_select_mode == 1);
    assert(sb->autocomplete.num_matches == 2);

    /* Check suggestions menu text for exact format: "1. abc.txt   2. abcd.txt" */
    assert(sb->autocomplete.menu_text[0] != '\0');
    assert(strstr(sb->autocomplete.menu_text, "1. abc.txt   2. abcd.txt") != NULL);

    /* Press '1' -> select abc.txt */
    int key_res = autocomplete_handle_key(sb, XK_1, '1');
    assert(key_res == 1);
    assert(strcmp(sb->input, "./myprog def.txt abc.txt") == 0);
    assert(sb->cursor_pos == (int)strlen("./myprog def.txt abc.txt"));
    assert(sb->autocomplete.in_select_mode == 0);

    /* Test selecting '2' on a new buffer */
    screen_buf_input_clear(sb);
    for (int i = 0; typed[i]; i++) screen_buf_input_insert(sb, typed[i]);
    autocomplete_handle_tab(sb);
    assert(sb->autocomplete.in_select_mode == 1);

    key_res = autocomplete_handle_key(sb, XK_2, '2');
    assert(key_res == 1);
    assert(strcmp(sb->input, "./myprog def.txt abcd.txt") == 0);
    assert(sb->autocomplete.in_select_mode == 0);

    screen_buf_destroy(sb);
    TEST_PASS();
}

static void test_lcp_extension_then_menu(void) {
    ScreenBuf *sb = screen_buf_create();
    assert(sb != NULL);
    screen_buf_set_cwd(sb, TEST_DIR);

    /* Type: "./myprog a" */
    const char *typed = "./myprog a";
    for (int i = 0; typed[i]; i++) {
        screen_buf_input_insert(sb, typed[i]);
    }

    /* Press Tab -> extends "a" to "abc" and shows numbered menu */
    int res = autocomplete_handle_tab(sb);
    assert(res == 1);
    assert(strcmp(sb->input, "./myprog abc") == 0);
    assert(sb->autocomplete.in_select_mode == 1);

    /* Press '1' -> becomes "./myprog abc.txt" */
    autocomplete_handle_key(sb, XK_1, '1');
    assert(strcmp(sb->input, "./myprog abc.txt") == 0);

    screen_buf_destroy(sb);
    TEST_PASS();
}

static void test_menu_cleared_on_letter_or_escape(void) {
    ScreenBuf *sb = screen_buf_create();
    assert(sb != NULL);
    screen_buf_set_cwd(sb, TEST_DIR);

    /* 1. Test letter press cancels select mode and clears suggestions */
    const char *typed = "./myprog def.txt abc";
    for (int i = 0; typed[i]; i++) screen_buf_input_insert(sb, typed[i]);
    autocomplete_handle_tab(sb);
    assert(sb->autocomplete.in_select_mode == 1);
    assert(sb->autocomplete.menu_text[0] != '\0');

    /* Pressing letter 'x' should return 0 (not handled as choice), clear in_select_mode and menu_text */
    int key_res = autocomplete_handle_key(sb, XK_x, 'x');
    assert(key_res == 0);
    assert(sb->autocomplete.in_select_mode == 0);
    assert(sb->autocomplete.menu_text[0] == '\0');

    /* 2. Test Escape cancels and clears */
    autocomplete_handle_tab(sb);
    assert(sb->autocomplete.in_select_mode == 1);
    assert(sb->autocomplete.menu_text[0] != '\0');

    key_res = autocomplete_handle_key(sb, XK_Escape, 0);
    assert(key_res == 1);
    assert(sb->autocomplete.in_select_mode == 0);
    assert(sb->autocomplete.menu_text[0] == '\0');

    screen_buf_destroy(sb);
    TEST_PASS();
}

static void test_lexicographical_sorting(void) {
    /* Create out-of-order test files in a custom subdir */
    const char *sort_dir = "/tmp/test_autocomplete_sort";
    mkdir(sort_dir, 0755);

    char path[512];
    const char *files[] = {"zebra.txt", "alpha.txt", "mango.txt", "apple.txt"};
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s", sort_dir, files[i]);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "test\n"); fclose(f); }
    }

    AutocompleteState state;
    int count = autocomplete_scan(sort_dir, "", &state);
    assert(count == 4);
    assert(strcmp(state.matches[0], "alpha.txt") == 0);
    assert(strcmp(state.matches[1], "apple.txt") == 0);
    assert(strcmp(state.matches[2], "mango.txt") == 0);
    assert(strcmp(state.matches[3], "zebra.txt") == 0);

    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s", sort_dir, files[i]);
        unlink(path);
    }
    rmdir(sort_dir);

    TEST_PASS();
}

int main(void) {
    printf("\n=== Running File-Name Auto-Completion Unit Tests (Stage 10) ===\n");
    setup_test_files();

    test_word_boundary_detection();
    test_directory_scan_and_lcp();
    test_exact_single_match_completion();
    test_ambiguous_match_and_numbered_menu();
    test_lcp_extension_then_menu();
    test_menu_cleared_on_letter_or_escape();
    test_lexicographical_sorting();

    cleanup_test_files();
    printf("✓ All Stage 10 Auto-Completion unit tests passed!\n\n");
    return 0;
}
