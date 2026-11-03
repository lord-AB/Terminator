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

static void unescape_word(const char *src, int src_len, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0) dst[0] = '\0';
        return;
    }
    size_t d = 0;
    for (int s = 0; s < src_len && d < dst_size - 1; s++) {
        if (src[s] == '\\' && s + 1 < src_len) {
            s++;
            dst[d++] = src[s];
        } else if (src[s] != '\\') {
            dst[d++] = src[s];
        }
    }
    dst[d] = '\0';
}

static void escape_path(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0) dst[0] = '\0';
        return;
    }
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d < dst_size - 1; s++) {
        char c = src[s];
        if (c == ' ' || c == '\t' || c == '(' || c == ')' ||
            c == '&' || c == ';' || c == '|' || c == '<' ||
            c == '>' || c == '$' || c == '`' || c == '\\' ||
            c == '"' || c == '\'') {
            if (d + 2 < dst_size) {
                dst[d++] = '\\';
                dst[d++] = c;
            } else {
                break;
            }
        } else {
            dst[d++] = c;
        }
    }
    dst[d] = '\0';
}

static int check_is_dir(const char *cwd, const char *dir_part, const char *name) {
    char path[MAX_PATH_LEN * 2];
    if (dir_part && dir_part[0] == '/') {
        snprintf(path, sizeof(path), "%s%s", dir_part, name);
    } else if (dir_part && dir_part[0] == '~' && (dir_part[1] == '/' || dir_part[1] == '\0')) {
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s%s%s", home ? home : "", dir_part + 1, name);
    } else if (cwd && cwd[0] != '\0') {
        if (dir_part && dir_part[0] != '\0') {
            snprintf(path, sizeof(path), "%s/%s%s", cwd, dir_part, name);
        } else {
            snprintf(path, sizeof(path), "%s/%s", cwd, name);
        }
    } else {
        if (dir_part && dir_part[0] != '\0') {
            snprintf(path, sizeof(path), "%s%s", dir_part, name);
        } else {
            snprintf(path, sizeof(path), "%s", name);
        }
    }

    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }
    return 0;
}

int autocomplete_find_word_ex(const char *input, int cursor_pos,
                              int *out_start, int *out_len, int *out_quote) {
    if (!input || cursor_pos < 0 || !out_start || !out_len) return -1;

    int cur = cursor_pos;
    int len = (int)strlen(input);
    if (cur > len) {
        cur = len;
    }

    int i = 0;
    int token_start = 0;
    int in_token = 0;
    int quote = 0;
    int quote_pos = -1;

    while (i < cur) {
        char c = input[i];

        if (quote == '\'') {
            if (c == '\'') {
                quote = 0;
                quote_pos = -1;
            }
            i++;
            continue;
        }

        if (quote == '"') {
            if (c == '\\') {
                if (i + 1 < cur) {
                    i += 2;
                } else {
                    i++;
                }
                continue;
            }
            if (c == '"') {
                quote = 0;
                quote_pos = -1;
            }
            i++;
            continue;
        }

        /* Outside quotes */
        if (c == '\\') {
            if (!in_token) {
                token_start = i;
                in_token = 1;
            }
            if (i + 1 < cur) {
                i += 2;
            } else {
                i++;
            }
            continue;
        }

        if (c == '\'') {
            quote = '\'';
            quote_pos = i;
            if (!in_token) {
                token_start = i;
                in_token = 1;
            }
            i++;
            continue;
        }

        if (c == '"') {
            quote = '"';
            quote_pos = i;
            if (!in_token) {
                token_start = i;
                in_token = 1;
            }
            i++;
            continue;
        }

        if (is_word_delim(c)) {
            in_token = 0;
            i++;
            continue;
        }

        /* Regular character */
        if (!in_token) {
            token_start = i;
            in_token = 1;
        }
        i++;
    }

    if (quote != 0 && quote_pos >= 0) {
        /* Inside quotes: word begins after the quote */
        *out_start = quote_pos + 1;
        *out_len = cur - (quote_pos + 1);
        if (out_quote) *out_quote = quote;
    } else if (in_token) {
        *out_start = token_start;
        *out_len = cur - token_start;
        if (out_quote) *out_quote = 0;
    } else {
        *out_start = cur;
        *out_len = 0;
        if (out_quote) *out_quote = 0;
    }

    return 0;
}

int autocomplete_find_word(const char *input, int cursor_pos, int *out_start, int *out_len) {
    return autocomplete_find_word_ex(input, cursor_pos, out_start, out_len, NULL);
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
    state->quote_char = 0;
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

    int word_start = 0, word_len = 0, quote_char = 0;
    if (autocomplete_find_word_ex(sb->input, sb->cursor_pos, &word_start, &word_len, &quote_char) < 0) {
        return 0;
    }

    char raw_word[MAX_PATH_LEN];
    if (word_len >= (int)sizeof(raw_word)) word_len = sizeof(raw_word) - 1;
    strncpy(raw_word, &sb->input[word_start], word_len);
    raw_word[word_len] = '\0';

    char word[MAX_PATH_LEN];
    if (quote_char == 0) {
        unescape_word(raw_word, word_len, word, sizeof(word));
    } else {
        strncpy(word, raw_word, sizeof(word) - 1);
        word[sizeof(word) - 1] = '\0';
    }

    int num = autocomplete_scan(sb->cwd, word, &sb->autocomplete);
    TERM_LOG("autocomplete: found %d matches for word '%s'", num, word);
    if (num == 0) {
        /* No match: do nothing */
        return 0;
    }

    sb->autocomplete.quote_char = quote_char;

    if (num == 1) {
        /* Exactly one match: complete it */
        int is_dir = check_is_dir(sb->cwd, sb->autocomplete.dir_part, sb->autocomplete.matches[0]);
        char raw_replacement[MAX_PATH_LEN * 2];
        if (is_dir) {
            snprintf(raw_replacement, sizeof(raw_replacement), "%s%s/",
                     sb->autocomplete.dir_part, sb->autocomplete.matches[0]);
        } else {
            snprintf(raw_replacement, sizeof(raw_replacement), "%s%s",
                     sb->autocomplete.dir_part, sb->autocomplete.matches[0]);
        }

        char replacement[MAX_PATH_LEN * 4];
        if (quote_char == 0) {
            escape_path(raw_replacement, replacement, sizeof(replacement));
        } else {
            strncpy(replacement, raw_replacement, sizeof(replacement) - 2);
            replacement[sizeof(replacement) - 2] = '\0';
            int has_closing = (sb->cursor_pos < (int)strlen(sb->input) &&
                               sb->input[sb->cursor_pos] == (char)quote_char);
            if (!is_dir && !has_closing) {
                size_t rlen = strlen(replacement);
                replacement[rlen] = (char)quote_char;
                replacement[rlen + 1] = '\0';
            }
        }

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
        char raw_replacement[MAX_PATH_LEN * 2];
        snprintf(raw_replacement, sizeof(raw_replacement), "%s%s",
                 sb->autocomplete.dir_part, lcp);

        char replacement[MAX_PATH_LEN * 4];
        if (quote_char == 0) {
            escape_path(raw_replacement, replacement, sizeof(replacement));
        } else {
            strncpy(replacement, raw_replacement, sizeof(replacement) - 1);
            replacement[sizeof(replacement) - 1] = '\0';
        }

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
            TERM_LOG("autocomplete: user selected #%d (%s)", choice + 1, sb->autocomplete.matches[choice]);
            int is_dir = check_is_dir(sb->cwd, sb->autocomplete.dir_part,
                                      sb->autocomplete.matches[choice]);
            char raw_replacement[MAX_PATH_LEN * 2];
            if (is_dir) {
                snprintf(raw_replacement, sizeof(raw_replacement), "%s%s/",
                         sb->autocomplete.dir_part, sb->autocomplete.matches[choice]);
            } else {
                snprintf(raw_replacement, sizeof(raw_replacement), "%s%s",
                         sb->autocomplete.dir_part, sb->autocomplete.matches[choice]);
            }

            char replacement[MAX_PATH_LEN * 4];
            if (sb->autocomplete.quote_char == 0) {
                escape_path(raw_replacement, replacement, sizeof(replacement));
            } else {
                strncpy(replacement, raw_replacement, sizeof(replacement) - 2);
                replacement[sizeof(replacement) - 2] = '\0';
                int has_closing = (sb->cursor_pos < (int)strlen(sb->input) &&
                                   sb->input[sb->cursor_pos] == (char)sb->autocomplete.quote_char);
                if (!is_dir && !has_closing) {
                    size_t rlen = strlen(replacement);
                    replacement[rlen] = (char)sb->autocomplete.quote_char;
                    replacement[rlen + 1] = '\0';
                }
            }

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
