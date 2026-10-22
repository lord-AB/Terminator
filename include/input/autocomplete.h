/*
 * autocomplete.h — Tab-key file-name auto-completion and numbered menu selection
 */
#ifndef INPUT_AUTOCOMPLETE_H
#define INPUT_AUTOCOMPLETE_H

#include "common.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>

#define AUTOCOMPLETE_MAX_MATCHES 32
#define AUTOCOMPLETE_MENU_MAXLEN 2048

typedef struct {
    int  in_select_mode;                                  /* 1 if waiting for user choice (1..N) */
    char matches[AUTOCOMPLETE_MAX_MATCHES][MAX_PATH_LEN]; /* matching filenames */
    int  num_matches;
    int  word_start;                                      /* start index of word in input */
    int  word_len;                                        /* length of prefix in input */
    char dir_part[MAX_PATH_LEN];                          /* directory prefix if any (e.g. "dir/") */
    char menu_text[AUTOCOMPLETE_MENU_MAXLEN];             /* suggestion menu formatted text */
} AutocompleteState;

typedef struct ScreenBuf ScreenBuf;

/*
 * Cancels autocomplete selection mode and clears the suggestion popup.
 */
void autocomplete_cancel(AutocompleteState *state);

/*
 * Finds the boundary of the word immediately before cursor_pos.
 * Returns 0 on success, -1 if no word before cursor.
 */
int autocomplete_find_word(const char *input, int cursor_pos, int *out_start, int *out_len);

/*
 * Scans directory for entries matching the word prefix.
 * Stores matches in state->matches and sets state->num_matches.
 * Returns number of matches found.
 */
int autocomplete_scan(const char *cwd, const char *word, AutocompleteState *state);

/*
 * Computes Longest Common Prefix (LCP) of all matches.
 * Returns length of LCP.
 */
int autocomplete_compute_lcp(const AutocompleteState *state, char *out_lcp, size_t max_len);

/*
 * Handles Tab key press on the active ScreenBuf.
 * Returns 1 if handled/completed/menu shown, 0 otherwise.
 */
int autocomplete_handle_tab(ScreenBuf *sb);

/*
 * Handles key press when in_select_mode is active.
 * Returns 1 if the key selected a menu choice (1..N), 0 if select mode was cancelled.
 */
int autocomplete_handle_key(ScreenBuf *sb, KeySym keysym, char ch);

#endif /* INPUT_AUTOCOMPLETE_H */
