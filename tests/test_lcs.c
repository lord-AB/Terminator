/*
 * test_lcs.c — Unit tests for Longest Common Substring and history search (Stage 9 / Task 9)
 */
#include "features/history_search.h"
#include "features/history_store.h"
#include "ipc/shm_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_PASS() printf("  ✓ %s passed\n", __func__)

static void test_lcs_basic(void) {
    /* Identical strings */
    assert(history_compute_lcs("hello", "hello") == 5);

    /* Full substring match */
    assert(history_compute_lcs("myprog", "./myprog < in.txt") == 6);
    assert(history_compute_lcs("myprog", "gcc -o myprog myprog.c") == 6);

    /* Disjoint strings */
    assert(history_compute_lcs("abc", "def") == 0);

    /* Short common character */
    assert(history_compute_lcs("xyz", "myprog") == 1); /* 'y' matches */

    /* Empty and NULL cases */
    assert(history_compute_lcs("", "myprog") == 0);
    assert(history_compute_lcs("myprog", "") == 0);
    assert(history_compute_lcs(NULL, "myprog") == 0);
    assert(history_compute_lcs("myprog", NULL) == 0);

    TEST_PASS();
}

static void test_history_search_transcript_cases(void) {
    SharedState *shm = shm_create(MODE_THREAD);
    assert(shm != NULL);

    /* Populate history matching Section 11 of design.md */
    shm_history_add(shm, "gcc -o myprog myprog.c");
    shm_history_add(shm, "./myprog < in.txt");

    /* Search 1: "myprog" */
    HistorySearchResult res1;
    int count1 = history_search_find(shm, "myprog", &res1);
    assert(count1 == 2);
    assert(res1.is_exact == 0);
    assert(res1.max_lcs_len == 6);
    /* Most recent first: "./myprog < in.txt" then "gcc -o myprog myprog.c" */
    assert(strcmp(res1.matches[0], "./myprog < in.txt") == 0);
    assert(strcmp(res1.matches[1], "gcc -o myprog myprog.c") == 0);

    char out1[1024];
    history_search_format(&res1, out1, sizeof(out1));
    assert(strcmp(out1, "./myprog < in.txt\ngcc -o myprog myprog.c\n") == 0);

    /* Search 2: "xyz" */
    HistorySearchResult res2;
    int count2 = history_search_find(shm, "xyz", &res2);
    assert(count2 == 0);
    assert(res2.max_lcs_len <= 2);

    char out2[1024];
    history_search_format(&res2, out2, sizeof(out2));
    assert(strcmp(out2, "No match for search term in history\n") == 0);

    /* Search 3: Exact match */
    shm_history_add(shm, "ls -la");
    shm_history_add(shm, "echo hello");
    shm_history_add(shm, "ls -la");

    HistorySearchResult res3;
    int count3 = history_search_find(shm, "ls -la", &res3);
    assert(count3 == 1);
    assert(res3.is_exact == 1);
    assert(strcmp(res3.matches[0], "ls -la") == 0);

    shm_destroy(shm, MODE_THREAD);
    TEST_PASS();
}

static void test_history_store_persistence(void) {
    const char *tmp_hist = "/tmp/test_terminator_history";
    unlink(tmp_hist);

    SharedState *shm1 = shm_create(MODE_THREAD);
    assert(shm1 != NULL);

    /* Append commands to store */
    assert(history_store_append(shm1, "cmd 1", tmp_hist) == 0);
    assert(history_store_append(shm1, "cmd 2", tmp_hist) == 0);
    assert(history_store_append(shm1, "cmd 3", tmp_hist) == 0);
    assert(shm_history_count(shm1) == 3);

    shm_destroy(shm1, MODE_THREAD);

    /* Load into a fresh shared state */
    SharedState *shm2 = shm_create(MODE_THREAD);
    assert(shm2 != NULL);
    int loaded = history_store_load(shm2, tmp_hist);
    assert(loaded == 3);
    assert(shm_history_count(shm2) == 3);
    assert(strcmp(shm_history_get(shm2, 0), "cmd 1") == 0);
    assert(strcmp(shm_history_get(shm2, 1), "cmd 2") == 0);
    assert(strcmp(shm_history_get(shm2, 2), "cmd 3") == 0);

    shm_destroy(shm2, MODE_THREAD);
    unlink(tmp_hist);
    TEST_PASS();
}

int main(void) {
    printf("\n=== Running Searchable History & LCS Unit Tests (Stage 9) ===\n");
    test_lcs_basic();
    test_history_search_transcript_cases();
    test_history_store_persistence();
    printf("✓ All Stage 9 History & LCS tests passed!\n\n");
    return 0;
}
