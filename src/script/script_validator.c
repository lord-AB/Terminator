/*
 * script_validator.c — Pre-execution syntax checker for bash scripts
 */
#include "script/script_validator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_STACK_DEPTH 128

typedef struct {
    int line;
    int has_then;
    int in_else;
} IfFrame;

static void set_error(ScriptValidationResult *res, int line, const char *msg) {
    if (!res) return;
    res->is_valid = 0;
    res->error_line = line;
    strncpy(res->error_msg, msg, sizeof(res->error_msg) - 1);
    res->error_msg[sizeof(res->error_msg) - 1] = '\0';
}

int script_validator_check_text(const char *text, ScriptValidationResult *out_res) {
    if (!out_res) return -1;
    out_res->is_valid = 1;
    out_res->error_line = 0;
    out_res->error_msg[0] = '\0';

    if (!text) return 0;

    IfFrame stack[MAX_STACK_DEPTH];
    int stack_top = 0;

    int line_num = 1;
    int in_single_quote = 0;
    int in_double_quote = 0;
    int quote_start_line = 0;

    const char *p = text;

    while (*p) {
        /* Read one line */
        const char *line_start = p;
        while (*p && *p != '\n') {
            p++;
        }
        size_t line_len = p - line_start;
        if (*p == '\n') p++; /* consume newline */

        char line[MAX_CMD_LEN];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        strncpy(line, line_start, line_len);
        line[line_len] = '\0';

        /* Scan line for tokens while tracking quotes */
        size_t i = 0;
        while (i < line_len) {
            char c = line[i];

            /* Handle single quote */
            if (c == '\'' && !in_double_quote) {
                if (!in_single_quote) {
                    in_single_quote = 1;
                    quote_start_line = line_num;
                } else {
                    in_single_quote = 0;
                }
                i++;
                continue;
            }

            /* Handle double quote (respecting \") */
            if (c == '"' && !in_single_quote) {
                if (i > 0 && line[i - 1] == '\\') {
                    /* escaped quote */
                    i++;
                    continue;
                }
                if (!in_double_quote) {
                    in_double_quote = 1;
                    quote_start_line = line_num;
                } else {
                    in_double_quote = 0;
                }
                i++;
                continue;
            }

            /* If inside quotes, ignore bash syntax tokens */
            if (in_single_quote || in_double_quote) {
                i++;
                continue;
            }

            /* Comment handling outside quotes */
            if (c == '#') {
                /* Rest of line is comment */
                break;
            }

            /* Skip whitespace and semicolons */
            if (isspace((unsigned char)c) || c == ';') {
                i++;
                continue;
            }

            /* Check keyword at token boundary */
            int at_token_start = (i == 0 || isspace((unsigned char)line[i - 1]) || line[i - 1] == ';');

            if (at_token_start) {
                /* Check 'if' */
                if (strncmp(&line[i], "if", 2) == 0 && (!line[i + 2] || isspace((unsigned char)line[i + 2]) || line[i + 2] == '[' || line[i + 2] == ';')) {
                    if (stack_top >= MAX_STACK_DEPTH) {
                        set_error(out_res, line_num, "syntax error: maximum if nesting depth exceeded");
                        return -1;
                    }
                    stack[stack_top].line = line_num;
                    stack[stack_top].has_then = 0;
                    stack[stack_top].in_else = 0;
                    stack_top++;
                    i += 2;
                    continue;
                }

                /* Check 'then' */
                if (strncmp(&line[i], "then", 4) == 0 && (!line[i + 4] || isspace((unsigned char)line[i + 4]) || line[i + 4] == ';')) {
                    if (stack_top == 0 || stack[stack_top - 1].in_else || stack[stack_top - 1].has_then) {
                        set_error(out_res, line_num, "syntax error near unexpected token 'then'");
                        return -1;
                    }
                    stack[stack_top - 1].has_then = 1;
                    i += 4;
                    continue;
                }

                /* Check 'elif' */
                if (strncmp(&line[i], "elif", 4) == 0 && (!line[i + 4] || isspace((unsigned char)line[i + 4]) || line[i + 4] == '[' || line[i + 4] == ';')) {
                    if (stack_top == 0 || stack[stack_top - 1].in_else || !stack[stack_top - 1].has_then) {
                        set_error(out_res, line_num, "syntax error near unexpected token 'elif'");
                        return -1;
                    }
                    stack[stack_top - 1].has_then = 0;
                    i += 4;
                    continue;
                }

                /* Check 'else' */
                if (strncmp(&line[i], "else", 4) == 0 && (!line[i + 4] || isspace((unsigned char)line[i + 4]) || line[i + 4] == ';')) {
                    if (stack_top == 0 || stack[stack_top - 1].in_else || !stack[stack_top - 1].has_then) {
                        set_error(out_res, line_num, "syntax error near unexpected token 'else'");
                        return -1;
                    }
                    stack[stack_top - 1].in_else = 1;
                    i += 4;
                    continue;
                }

                /* Check 'fi' */
                if (strncmp(&line[i], "fi", 2) == 0 && (!line[i + 2] || isspace((unsigned char)line[i + 2]) || line[i + 2] == ';')) {
                    if (stack_top == 0) {
                        set_error(out_res, line_num, "syntax error near unexpected token 'fi'");
                        return -1;
                    }
                    if (!stack[stack_top - 1].has_then) {
                        set_error(out_res, line_num, "syntax error: expected 'then' before 'fi'");
                        return -1;
                    }
                    stack_top--;
                    i += 2;
                    continue;
                }
            }

            /* Advance past non-keyword token */
            while (i < line_len && !isspace((unsigned char)line[i]) && line[i] != ';' && line[i] != '\'' && line[i] != '"') {
                i++;
            }
        }

        line_num++;
    }

    if (in_single_quote || in_double_quote) {
        char err[128];
        snprintf(err, sizeof(err), "syntax error: unexpected EOF while looking for matching %s quote",
                 in_single_quote ? "single" : "double");
        set_error(out_res, quote_start_line, err);
        return -1;
    }

    if (stack_top > 0) {
        char err[128];
        snprintf(err, sizeof(err), "syntax error: unexpected end of file, missing 'fi' for 'if' on line %d",
                 stack[stack_top - 1].line);
        set_error(out_res, line_num - 1, err);
        return -1;
    }

    return 0;
}

int script_validator_check_file(const char *filepath, ScriptValidationResult *out_res) {
    if (!filepath || !out_res) return -1;

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        set_error(out_res, 0, "No such file or directory");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        set_error(out_res, 0, "Failed to read file size");
        return -1;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        set_error(out_res, 0, "Memory allocation error");
        return -1;
    }

    size_t n = fread(buf, 1, (size_t)size, fp);
    buf[n] = '\0';
    fclose(fp);

    int res = script_validator_check_text(buf, out_res);
    free(buf);
    return res;
}
