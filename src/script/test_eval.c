/*
 * test_eval.c — POSIX [ ... ] condition evaluator
 */
#include "script/test_eval.h"
#include "parser/expand.h"
#include "parser/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

static void resolve_path(ShellContext *ctx, const char *raw_path, char *out_path, size_t out_size) {
    if (!raw_path || out_size == 0) return;

    if (raw_path[0] == '/' || !ctx || ctx->cwd[0] == '\0') {
        strncpy(out_path, raw_path, out_size - 1);
        out_path[out_size - 1] = '\0';
    } else if (raw_path[0] == '~' && (raw_path[1] == '/' || raw_path[1] == '\0')) {
        const char *home = getenv("HOME");
        snprintf(out_path, out_size, "%s%s", home ? home : "", raw_path + 1);
    } else {
        snprintf(out_path, out_size, "%s/%s", ctx->cwd, raw_path);
    }
}

int test_eval_tokens(ShellContext *ctx, int argc, char *argv[]) {
    if (argc <= 0 || !argv || !argv[0]) {
        return 1; /* empty test is false */
    }

    /* 1. Handle logical -o (OR) at lowest precedence */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            int left_res = test_eval_tokens(ctx, i, argv);
            if (left_res == 0) return 0; /* Short-circuit OR */
            return test_eval_tokens(ctx, argc - i - 1, argv + i + 1);
        }
    }

    /* 2. Handle logical -a (AND) */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            int left_res = test_eval_tokens(ctx, i, argv);
            if (left_res != 0) return 1; /* Short-circuit AND */
            return test_eval_tokens(ctx, argc - i - 1, argv + i + 1);
        }
    }

    /* 3. Handle inversion '!' */
    if (strcmp(argv[0], "!") == 0) {
        int sub_res = test_eval_tokens(ctx, argc - 1, argv + 1);
        return (sub_res == 0) ? 1 : 0;
    }

    /* 4. Single argument: non-empty string is true */
    if (argc == 1) {
        return (argv[0][0] != '\0') ? 0 : 1;
    }

    /* 5. Two arguments: unary operators */
    if (argc == 2) {
        const char *op = argv[0];
        const char *arg = argv[1];

        if (strcmp(op, "-z") == 0) {
            return (arg[0] == '\0') ? 0 : 1;
        }
        if (strcmp(op, "-n") == 0) {
            return (arg[0] != '\0') ? 0 : 1;
        }

        /* File existence & type operators */
        char full_path[MAX_PATH_LEN];
        resolve_path(ctx, arg, full_path, sizeof(full_path));

        struct stat st;
        int st_res = stat(full_path, &st);

        if (strcmp(op, "-f") == 0) {
            return (st_res == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        }
        if (strcmp(op, "-d") == 0) {
            return (st_res == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        }
        if (strcmp(op, "-e") == 0) {
            return (st_res == 0) ? 0 : 1;
        }
        if (strcmp(op, "-s") == 0) {
            return (st_res == 0 && st.st_size > 0) ? 0 : 1;
        }
        if (strcmp(op, "-r") == 0) {
            return (access(full_path, R_OK) == 0) ? 0 : 1;
        }
        if (strcmp(op, "-w") == 0) {
            return (access(full_path, W_OK) == 0) ? 0 : 1;
        }
        if (strcmp(op, "-x") == 0) {
            return (access(full_path, X_OK) == 0) ? 0 : 1;
        }

        return 1; /* Unknown unary operator */
    }

    /* 6. Three arguments: binary comparisons */
    if (argc == 3) {
        const char *left = argv[0];
        const char *op = argv[1];
        const char *right = argv[2];

        /* String comparisons */
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            return (strcmp(left, right) == 0) ? 0 : 1;
        }
        if (strcmp(op, "!=") == 0) {
            return (strcmp(left, right) != 0) ? 0 : 1;
        }

        /* Integer comparisons */
        long long v1 = atoll(left);
        long long v2 = atoll(right);

        if (strcmp(op, "-eq") == 0) return (v1 == v2) ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return (v1 != v2) ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return (v1 > v2) ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return (v1 >= v2) ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return (v1 < v2) ? 0 : 1;
        if (strcmp(op, "-le") == 0) return (v1 <= v2) ? 0 : 1;
    }

    return 1; /* Default to false on unhandled syntax */
}

int test_eval_expression(ShellContext *ctx, const char *raw_expr) {
    if (!raw_expr) return 1;

    /* Skip leading whitespace */
    const char *p = raw_expr;
    while (*p && isspace((unsigned char)*p)) p++;

    /* If starts with '[', skip it */
    if (*p == '[') p++;

    /* Copy into local buffer */
    char buf[MAX_CMD_LEN];
    strncpy(buf, p, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Strip trailing whitespace and optional ']' */
    size_t len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[--len] = '\0';
    }
    if (len > 0 && buf[len - 1] == ']') {
        buf[--len] = '\0';
    }
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[--len] = '\0';
    }

    /* 1. Expand variables in the expression ($1, $VAR, etc.) */
    char expanded[MAX_CMD_LEN];
    if (expand_variables(ctx, buf, expanded, sizeof(expanded)) < 0) {
        return 1;
    }

    /* 2. Tokenize respecting quotes */
    char *tokens[MAX_ARGS];
    int argc = lexer_tokenize(expanded, tokens, MAX_ARGS);
    if (argc < 0) {
        return 1;
    }

    int res = test_eval_tokens(ctx, argc, tokens);
    lexer_free_tokens(tokens, argc);

    return res;
}
