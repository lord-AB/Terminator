/*
 * history_search.c — Exact match and Longest Common Substring (LCS) history search
 */
#include "features/history_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int history_compute_lcs(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int m = (int)strlen(s1);
    int n = (int)strlen(s2);
    if (m == 0 || n == 0) return 0;

    /* Ensure s2 is the shorter string so DP row size (n + 1) is minimized */
    if (m < n) {
        const char *t = s1; s1 = s2; s2 = t;
        int tn = m; m = n; n = tn;
    }

    #define STACK_DP_CAP 512
    int stack_prev[STACK_DP_CAP];
    int stack_curr[STACK_DP_CAP];
    int *prev = stack_prev;
    int *curr = stack_curr;
    int allocated = 0;

    if (n + 1 > STACK_DP_CAP) {
        prev = (int *)calloc(n + 1, sizeof(int));
        curr = (int *)calloc(n + 1, sizeof(int));
        if (!prev || !curr) {
            free(prev);
            free(curr);
            return 0;
        }
        allocated = 1;
    } else {
        memset(stack_prev, 0, (n + 1) * sizeof(int));
        memset(stack_curr, 0, (n + 1) * sizeof(int));
    }

    int max_len = 0;
    for (int i = 1; i <= m; i++) {
        for (int j = 1; j <= n; j++) {
            if (s1[i - 1] == s2[j - 1]) {
                curr[j] = prev[j - 1] + 1;
                if (curr[j] > max_len) {
                    max_len = curr[j];
                }
            } else {
                curr[j] = 0;
            }
        }
        int *temp = prev;
        prev = curr;
        curr = temp;
        memset(curr, 0, (n + 1) * sizeof(int));
    }

    if (allocated) {
        free(prev);
        free(curr);
    }
    #undef STACK_DP_CAP
    return max_len;
}

static int is_duplicate(const HistorySearchResult *result, const char *cmd) {
    for (int i = 0; i < result->count; i++) {
        if (strcmp(result->matches[i], cmd) == 0) {
            return 1;
        }
    }
    return 0;
}

int history_search_find(SharedState *shm, const char *term, HistorySearchResult *result) {
    if (!result) return 0;
    memset(result, 0, sizeof(HistorySearchResult));
    if (!shm || !term || term[0] == '\0') return 0;

    shm_lock(shm);

    int total = shm->count;
    int stored = total < HISTORY_CAPACITY ? total : HISTORY_CAPACITY;
    if (stored <= 0) {
        shm_unlock(shm);
        return 0;
    }

    /* Helper macro to get command string at logical index (0 = oldest, stored-1 = newest) */
    #define GET_CMD(idx) \
        ((shm->count <= HISTORY_CAPACITY) ? \
            shm->entries[(idx)].cmd : \
            shm->entries[(shm->head + (idx)) % HISTORY_CAPACITY].cmd)

    /* ── 1. Check for exact match (from most recent to oldest) ──────── */
    for (int i = stored - 1; i >= 0; i--) {
        const char *cmd = GET_CMD(i);
        if (cmd && strcmp(cmd, term) == 0) {
            strncpy(result->matches[0], cmd, MAX_CMD_LEN - 1);
            result->matches[0][MAX_CMD_LEN - 1] = '\0';
            result->count = 1;
            result->max_lcs_len = (int)strlen(term);
            result->is_exact = 1;
            shm_unlock(shm);
            return 1;
        }
    }

    /* ── 2. Compute Longest Common Substring (LCS) for all commands ─── */
    int stack_lcs[1024];
    int *lcs_lens = stack_lcs;
    int lcs_allocated = 0;
    if (stored > 1024) {
        lcs_lens = (int *)malloc(stored * sizeof(int));
        if (!lcs_lens) {
            shm_unlock(shm);
            return 0;
        }
        lcs_allocated = 1;
    }

    int max_len = 0;
    for (int i = stored - 1; i >= 0; i--) {
        const char *cmd = GET_CMD(i);
        if (cmd && cmd[0] != '\0') {
            int lcs = history_compute_lcs(term, cmd);
            lcs_lens[i] = lcs;
            if (lcs > max_len) {
                max_len = lcs;
            }
        } else {
            lcs_lens[i] = 0;
        }
    }

    /* ── 3. If max_len > 2, collect all commands matching max_len ───── */
    if (max_len > 2) {
        for (int i = stored - 1; i >= 0; i--) {
            if (lcs_lens[i] == max_len) {
                const char *cmd = GET_CMD(i);
                if (cmd && !is_duplicate(result, cmd)) {
                    if (result->count < HISTORY_SEARCH_MAX_MATCHES) {
                        strncpy(result->matches[result->count], cmd, MAX_CMD_LEN - 1);
                        result->matches[result->count][MAX_CMD_LEN - 1] = '\0';
                        result->count++;
                    }
                }
            }
        }
    }

    result->max_lcs_len = max_len;
    result->is_exact = 0;

    if (lcs_allocated) {
        free(lcs_lens);
    }
    shm_unlock(shm);
    #undef GET_CMD

    return result->count;
}

int history_search_format(const HistorySearchResult *result, char *out_buf, size_t out_sz) {
    if (!out_buf || out_sz == 0) return -1;
    out_buf[0] = '\0';

    if (!result || result->count == 0) {
        snprintf(out_buf, out_sz, "No match for search term in history\n");
        return 0;
    }

    size_t offset = 0;
    for (int i = 0; i < result->count; i++) {
        int n = snprintf(out_buf + offset, out_sz - offset, "%s\n", result->matches[i]);
        if (n < 0 || (size_t)n >= out_sz - offset) {
            break;
        }
        offset += n;
    }

    return (int)result->count;
}
