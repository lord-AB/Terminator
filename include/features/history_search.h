/*
 * history_search.h — Ctrl+R reverse history search via Exact Match & LCS algorithms
 */
#ifndef FEATURES_HISTORY_SEARCH_H
#define FEATURES_HISTORY_SEARCH_H

#include "common.h"
#include "ipc/shm_sync.h"

#define HISTORY_SEARCH_MAX_MATCHES 128

typedef struct {
    char matches[HISTORY_SEARCH_MAX_MATCHES][MAX_CMD_LEN];
    int  count;
    int  max_lcs_len;
    int  is_exact;
} HistorySearchResult;

/*
 * Computes length of the longest contiguous common substring between s1 and s2.
 */
int history_compute_lcs(const char *s1, const char *s2);

/*
 * Searches shared memory history for 'term' from most recent to oldest:
 * 1. Exact match: returns the most recent exact match if found.
 * 2. Longest Common Substring: finds command(s) sharing the maximum
 *    contiguous common substring if max_len > 2.
 * 3. No match otherwise.
 * Returns the number of matches found.
 */
int history_search_find(SharedState *shm, const char *term, HistorySearchResult *result);

/*
 * Formats the search result into out_buf:
 * - One or more commands followed by '\n'
 * - Or "No match for search term in history\n"
 */
int history_search_format(const HistorySearchResult *result, char *out_buf, size_t out_sz);

#endif /* FEATURES_HISTORY_SEARCH_H */
