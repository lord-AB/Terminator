/*
 * multiline.c — State machine checking quote/paren balance across newlines
 */
#include "parser/multiline.h"
#include <string.h>
#include <ctype.h>

int multiline_is_incomplete(const char *input) {
    if (!input || !*input) return 0;

    int in_single_quote = 0;
    int in_double_quote = 0;
    int in_backtick = 0;
    int paren_depth = 0;   /* depth of $( ... ) */

    const char *p = input;
    while (*p) {
        if (in_single_quote) {
            /* Inside single quotes, everything is literal until closing quote */
            if (*p == '\'') {
                in_single_quote = 0;
            }
            p++;
        } else if (in_double_quote) {
            /* Inside double quotes: \" is escaped, \\ is escaped, $(...) is parsed */
            if (*p == '\\') {
                if (*(p + 1) != '\0') {
                    p += 2; /* skip escape sequence */
                    continue;
                } else {
                    p++;
                    break;
                }
            } else if (*p == '"') {
                in_double_quote = 0;
                p++;
            } else if (*p == '$' && *(p + 1) == '(') {
                paren_depth++;
                p += 2;
            } else if (*p == ')' && paren_depth > 0) {
                paren_depth--;
                p++;
            } else if (*p == '`') {
                in_backtick = !in_backtick;
                p++;
            } else {
                p++;
            }
        } else {
            /* Outside quotes */
            if (*p == '\\') {
                if (*(p + 1) != '\0') {
                    p += 2; /* skip escape sequence */
                    continue;
                } else {
                    p++;
                    break;
                }
            } else if (*p == '\'') {
                in_single_quote = 1;
                p++;
            } else if (*p == '"') {
                in_double_quote = 1;
                p++;
            } else if (*p == '`') {
                in_backtick = !in_backtick;
                p++;
            } else if (*p == '$' && *(p + 1) == '(') {
                paren_depth++;
                p += 2;
            } else if (*p == ')' && paren_depth > 0) {
                paren_depth--;
                p++;
            } else {
                p++;
            }
        }
    }

    /* If quotes, backticks, or subshells are still open, it is incomplete */
    if (in_single_quote || in_double_quote || in_backtick || paren_depth > 0) {
        return 1;
    }

    /* Check for trailing backslash (line continuation) */
    size_t len = strlen(input);
    /* Ignore trailing whitespace */
    while (len > 0 && isspace((unsigned char)input[len - 1])) {
        len--;
    }

    if (len > 0) {
        size_t bslash_count = 0;
        ssize_t i = (ssize_t)len - 1;
        while (i >= 0 && input[i] == '\\') {
            bslash_count++;
            i--;
        }
        /* Odd number of trailing backslashes means line continuation */
        if (bslash_count % 2 == 1) {
            return 1;
        }
    }

    return 0;
}
