/*
 * lexer.c — Quote-aware tokenizer stripping quotes and separating operators
 */
#include "parser/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void lexer_free_tokens(char *tokens[], int count) {
    if (!tokens) return;
    for (int i = 0; i < count; i++) {
        if (tokens[i]) {
            free(tokens[i]);
            tokens[i] = NULL;
        }
    }
}

int lexer_tokenize(const char *input, char *tokens[], int max_tokens) {
    if (!input || !tokens || max_tokens <= 0) return 0;

    int count = 0;
    const char *p = input;
    char word[MAX_CMD_LEN];
    size_t w_len = 0;
    int in_word = 0;
    int in_single = 0;
    int in_double = 0;

    while (*p && count < max_tokens - 1) {
        char c = *p;

        /* 1. Inside single quotes */
        if (in_single) {
            if (c == '\'') {
                in_single = 0;
                p++;
            } else {
                if (w_len < sizeof(word) - 1) word[w_len++] = c;
                p++;
            }
            continue;
        }

        /* 2. Inside double quotes */
        if (in_double) {
            if (c == '\\') {
                char next = *(p + 1);
                if (next == '"' || next == '\\' || next == '$' || next == '`') {
                    if (w_len < sizeof(word) - 1) word[w_len++] = next;
                    p += 2;
                    continue;
                }
            }
            if (c == '"') {
                in_double = 0;
                p++;
            } else {
                if (w_len < sizeof(word) - 1) word[w_len++] = c;
                p++;
            }
            continue;
        }

        /* 3. Escaped character outside quotes */
        if (c == '\\') {
            in_word = 1;
            char next = *(p + 1);
            if (next != '\0') {
                if (w_len < sizeof(word) - 1) word[w_len++] = next;
                p += 2;
            } else {
                p++;
            }
            continue;
        }

        /* 4. Single quote open */
        if (c == '\'') {
            in_word = 1;
            in_single = 1;
            p++;
            continue;
        }

        /* 5. Double quote open */
        if (c == '"') {
            in_word = 1;
            in_double = 1;
            p++;
            continue;
        }

        /* 6. Shell operators: |, <, >, & */
        if (c == '|' || c == '<' || c == '>' || c == '&') {
            /* Finish any pending word token */
            if (in_word) {
                word[w_len] = '\0';
                tokens[count++] = strdup(word);
                w_len = 0;
                in_word = 0;
                if (count >= max_tokens - 1) break;
            }

            /* Recognize '>>' */
            if (c == '>' && *(p + 1) == '>') {
                tokens[count++] = strdup(">>");
                p += 2;
            } else {
                char op_str[2] = { c, '\0' };
                tokens[count++] = strdup(op_str);
                p++;
            }
            continue;
        }

        /* 7. Whitespace */
        if (isspace((unsigned char)c)) {
            if (in_word) {
                word[w_len] = '\0';
                tokens[count++] = strdup(word);
                w_len = 0;
                in_word = 0;
            }
            p++;
            continue;
        }

        /* 8. Regular character */
        in_word = 1;
        if (w_len < sizeof(word) - 1) word[w_len++] = c;
        p++;
    }

    if (in_word && count < max_tokens - 1) {
        word[w_len] = '\0';
        tokens[count++] = strdup(word);
    }

    tokens[count] = NULL;
    return count;
}
