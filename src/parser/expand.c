/*
 * expand.c — Variable ($VAR, $?, ${VAR}) and line continuation expansion
 */
#include "parser/expand.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int expand_variables(ShellContext *ctx, const char *input, char *output, size_t out_size) {
    if (!input || !output || out_size == 0) return -1;

    size_t out_idx = 0;
    int in_single_quote = 0;
    int in_double_quote = 0;
    const char *p = input;

    while (*p && out_idx < out_size - 1) {
        /* 1. Handle line continuation: backslash followed immediately by newline */
        if (*p == '\\' && *(p + 1) == '\n') {
            p += 2;
            continue;
        }

        /* 2. Single quote handling */
        if (*p == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            output[out_idx++] = *p++;
            continue;
        }

        /* 3. Double quote handling */
        if (*p == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            output[out_idx++] = *p++;
            continue;
        }

        /* 4. Escaped characters outside single quotes */
        if (*p == '\\' && !in_single_quote) {
            char next = *(p + 1);
            if (next == '$' || next == '"' || next == '\\' || next == '`') {
                /* Preserve backslash for lexer or pass escaped char */
                output[out_idx++] = *p++;
                if (*p && out_idx < out_size - 1) {
                    output[out_idx++] = *p++;
                }
                continue;
            }
        }

        /* 5. Variable expansion ($VAR, $?, ${VAR}) — only if not in single quotes */
        if (*p == '$' && !in_single_quote) {
            p++; /* consume '$' */

            if (*p == '?') {
                /* $? — exit status of last command */
                char num_buf[32];
                int last_status = ctx ? ctx->last_status : 0;
                int nlen = snprintf(num_buf, sizeof(num_buf), "%d", last_status);
                for (int i = 0; i < nlen && out_idx < out_size - 1; i++) {
                    output[out_idx++] = num_buf[i];
                }
                p++;
                continue;
            }

            if (*p == '{') {
                /* ${VAR} syntax */
                p++; /* consume '{' */
                char var_name[128];
                size_t v_idx = 0;
                while (*p && *p != '}' && v_idx < sizeof(var_name) - 1) {
                    var_name[v_idx++] = *p++;
                }
                var_name[v_idx] = '\0';
                if (*p == '}') p++; /* consume '}' */

                /* Look up value: first in local shell ctx, then environment */
                const char *val = ctx ? shell_ctx_get_var(ctx, var_name) : NULL;
                if (!val) val = getenv(var_name);

                if (val) {
                    while (*val && out_idx < out_size - 1) {
                        output[out_idx++] = *val++;
                    }
                }
                continue;
            }

            if (isalpha((unsigned char)*p) || *p == '_') {
                /* $VAR identifier */
                char var_name[128];
                size_t v_idx = 0;
                while ((isalnum((unsigned char)*p) || *p == '_') && v_idx < sizeof(var_name) - 1) {
                    var_name[v_idx++] = *p++;
                }
                var_name[v_idx] = '\0';

                /* Look up value */
                const char *val = ctx ? shell_ctx_get_var(ctx, var_name) : NULL;
                if (!val) val = getenv(var_name);

                if (val) {
                    while (*val && out_idx < out_size - 1) {
                        output[out_idx++] = *val++;
                    }
                }
                continue;
            }

            /* Lone '$' or not a variable identifier */
            output[out_idx++] = '$';
            continue;
        }

        /* 6. Regular character */
        output[out_idx++] = *p++;
    }

    output[out_idx] = '\0';
    return (out_idx < out_size) ? 0 : -1;
}
