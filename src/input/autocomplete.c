/*
 * autocomplete.c — File-name auto-completion and numbered menu selection
 */
#include "input/autocomplete.h"
#include "gui/screen_buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int is_word_delim(char ch) {
    return (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
            ch == ';' || ch == '|' || ch == '&' || ch == '<' || ch == '>');
}

int autocomplete_find_word(const char *input, int cursor_pos, int *out_start, int *out_len) {
    if (!input || cursor_pos < 0 || !out_start || !out_len) return -1;

    int cur = cursor_pos;
    if (cur > (int)strlen(input)) {
        cur = (int)strlen(input);
    }

    /* Walk backwards from cursor_pos to find word start */
    int start = cur;
    while (start > 0 && !is_word_delim(input[start - 1])) {
        start--;
    }

    *out_start = start;
    *out_len = cur - start;
    return 0;
}

static int match_compare(const void *a, const void *b) {
    const char *s1 = (const char *)a;
    const char *s2 = (const char *)b;
    return strcmp(s1, s2);
}

void autocomplete_cancel(AutocompleteState *state) {
    if (!state) return;
    state->in_select_mode = 0;
    state->menu_text[0] = '\0';
}

int autocomplete_scan(const char *cwd, const char *word, AutocompleteState *state) {
    if (!state) return 0;
    memset(state, 0, sizeof(AutocompleteState));
    if (!word) return 0;

    /* Split word into dir_part and file_prefix */
    const char *last_slash = strrchr(word, '/');
    char file_prefix[MAX_PATH_LEN] = {0};

    if (last_slash) {
        size_t dlen = last_slash - word + 1;
        if (dlen >= sizeof(state->dir_part)) dlen = sizeof(state->dir_part) - 1;
        strncpy(state->dir_part, word, dlen);
        state->dir_part[dlen] = '\0';
        strncpy(file_prefix, last_slash + 1, sizeof(file_prefix) - 1);
    } else {
        state->dir_part[0] = '\0';
        strncpy(file_prefix, word, sizeof(file_prefix) - 1);
    }

    /* Resolve directory path to scan */
    char scan_dir[MAX_PATH_LEN];
    if (state->dir_part[0] == '\0') {
        if (cwd && cwd[0] != '\0') {
            strncpy(scan_dir, cwd, sizeof(scan_dir) - 1);
            scan_dir[sizeof(scan_dir) - 1] = '\0';
        } else {
            strcpy(scan_dir, ".");
        }
    } else if (state->dir_part[0] == '/') {
        /* Absolute path */
        strncpy(scan_dir, state->dir_part, sizeof(scan_dir) - 1);
        scan_dir[sizeof(scan_dir) - 1] = '\0';
    } else if (state->dir_part[0] == '~' && (state->dir_part[1] == '/' || state->dir_part[1] == '\0')) {
        /* Home directory */
        const char *home = getenv("HOME");
        snprintf(scan_dir, sizeof(scan_dir), "%s%s", home ? home : "", state->dir_part + 1);
    } else {
        /* Relative to cwd */
        if (cwd && cwd[0] != '\0') {
            snprintf(scan_dir, sizeof(scan_dir), "%s/%s", cwd, state->dir_part);
        } else {
            strncpy(scan_dir, state->dir_part, sizeof(scan_dir) - 1);
            scan_dir[sizeof(scan_dir) - 1] = '\0';
        }
    }

    /* Strip trailing slashes for opendir (unless root "/") */
    size_t slen = strlen(scan_dir);
    while (slen > 1 && scan_dir[slen - 1] == '/') {
        scan_dir[--slen] = '\0';
    }

    DIR *dir = opendir(scan_dir);
    if (!dir) {
        return 0;
    }

    size_t prefix_len = strlen(file_prefix);
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip '.' and '..' unless prefix explicitly starts with '.' */
        if (file_prefix[0] != '.' && entry->d_name[0] == '.') {
            continue;
        }

        if (strncmp(entry->d_name, file_prefix, prefix_len) == 0) {
            if (state->num_matches < AUTOCOMPLETE_MAX_MATCHES) {
                strncpy(state->matches[state->num_matches], entry->d_name, AUTOCOMPLETE_NAME_MAX - 1);
                state->matches[state->num_matches][AUTOCOMPLETE_NAME_MAX - 1] = '\0';
                state->num_matches++;
            }
        }
    }

    closedir(dir);

    /* Sort matches alphabetically */
    if (state->num_matches > 1) {
        qsort(state->matches, state->num_matches, sizeof(state->matches[0]), match_compare);
    }

    return state->num_matches;
}

int autocomplete_compute_lcp(const AutocompleteState *state, char *out_lcp, size_t max_len) {
    if (!state || state->num_matches <= 0 || !out_lcp || max_len == 0) return 0;

    if (state->num_matches == 1) {
        strncpy(out_lcp, state->matches[0], max_len - 1);
        out_lcp[max_len - 1] = '\0';
        return (int)strlen(out_lcp);
    }

    int idx = 0;
    while ((size_t)idx < max_len - 1) {
        char c = state->matches[0][idx];
        if (c == '\0') break;

        int all_match = 1;
        for (int m = 1; m < state->num_matches; m++) {
            if (state->matches[m][idx] != c) {
                all_match = 0;
                break;
            }
        }

        if (!all_match) break;
        out_lcp[idx++] = c;
    }

    out_lcp[idx] = '\0';
    return idx;
}

int autocomplete_handle_tab(ScreenBuf *sb) {
    if (!sb) return 0;

    int word_start = 0, word_len = 0;
    if (autocomplete_find_word(sb->input, sb->cursor_pos, &word_start, &word_len) < 0) {
        return 0;
    }

    char word[MAX_PATH_LEN];
    if (word_len >= (int)sizeof(word)) word_len = sizeof(word) - 1;
    strncpy(word, &sb->input[word_start], word_len);
    word[word_len] = '\0';

    int num = autocomplete_scan(sb->cwd, word, &sb->autocomplete);
    if (num == 0) {
        /* No match: do nothing */
        return 0;
    }

    if (num == 1) {
        /* Exactly one match: complete it */
        char replacement[MAX_PATH_LEN * 2];
        snprintf(replacement, sizeof(replacement), "%s%s",
                 sb->autocomplete.dir_part, sb->autocomplete.matches[0]);

        screen_buf_input_replace_range(sb, word_start, word_len, replacement);
        sb->autocomplete.in_select_mode = 0;
        return 1;
    }

    /* Several matches: extend to LCP first */
    char lcp[MAX_PATH_LEN];
    int lcp_len = autocomplete_compute_lcp(&sb->autocomplete, lcp, sizeof(lcp));

    /* Get filename prefix part of word */
    const char *last_slash = strrchr(word, '/');
    const char *file_prefix = last_slash ? last_slash + 1 : word;
    int prefix_len = (int)strlen(file_prefix);

    if (lcp_len > prefix_len) {
        char replacement[MAX_PATH_LEN * 2];
        snprintf(replacement, sizeof(replacement), "%s%s",
                 sb->autocomplete.dir_part, lcp);

        screen_buf_input_replace_range(sb, word_start, word_len, replacement);
        word_len = (int)strlen(replacement);
    }

    /* Format numbered menu: "1. abc.txt   2. abcd.txt" */
    char menu[LINE_MAXLEN * 4] = {0};
    size_t offset = 0;
    for (int i = 0; i < num; i++) {
        int n = snprintf(menu + offset, sizeof(menu) - offset, "%d. %s   ",
                         i + 1, sb->autocomplete.matches[i]);
        if (n < 0 || (size_t)n >= sizeof(menu) - offset) break;
        offset += n;
    }

    /* Strip trailing spaces */
    while (offset > 0 && menu[offset - 1] == ' ') {
        menu[--offset] = '\0';
    }

    /* Store menu text to be displayed under the cursor/input line */
    strncpy(sb->autocomplete.menu_text, menu, sizeof(sb->autocomplete.menu_text) - 1);
    sb->autocomplete.menu_text[sizeof(sb->autocomplete.menu_text) - 1] = '\0';

    /* Enter completion select mode */
    sb->autocomplete.in_select_mode = 1;
    sb->autocomplete.word_start = word_start;
    sb->autocomplete.word_len = word_len;

    return 1;
}

int autocomplete_handle_key(ScreenBuf *sb, KeySym keysym, char ch) {
    if (!sb || !sb->autocomplete.in_select_mode) return 0;

    if (keysym == XK_Escape) {
        autocomplete_cancel(&sb->autocomplete);
        return 1;
    }

    /* Check for number choice (1..N) */
    if (ch >= '1' && ch <= '9') {
        int choice = ch - '1';
        if (choice < sb->autocomplete.num_matches) {
            char replacement[MAX_PATH_LEN * 2];
            snprintf(replacement, sizeof(replacement), "%s%s",
                     sb->autocomplete.dir_part, sb->autocomplete.matches[choice]);

            screen_buf_input_replace_range(sb, sb->autocomplete.word_start,
                                          sb->autocomplete.word_len, replacement);
            autocomplete_cancel(&sb->autocomplete);
            return 1;
        }
    }

    /* Any other key cancels select mode and is cleared immediately */
    autocomplete_cancel(&sb->autocomplete);
    return 0;
}
