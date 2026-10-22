/*
 * test_eval.h — POSIX [ ... ] condition evaluator
 */
#ifndef SCRIPT_TEST_EVAL_H
#define SCRIPT_TEST_EVAL_H

#include "exec/shell_ctx.h"

/*
 * Evaluates a test expression:
 * - Unary: -f, -d, -e, -s, -z, -n
 * - Binary string: =, ==, !=
 * - Binary integer: -eq, -ne, -gt, -ge, -lt, -le
 * - Inversion: !
 * - Logical: -a, -o
 *
 * Paths (-f, -d, etc.) are resolved relative to ctx->cwd.
 * Returns 0 if TRUE (bash exit status 0 = success),
 *         1 if FALSE (bash exit status 1 = failure),
 *        -1 if syntax error.
 */
int test_eval_expression(ShellContext *ctx, const char *raw_expr);

/*
 * Evaluates tokenized arguments (excluding surrounding '[' and ']').
 */
int test_eval_tokens(ShellContext *ctx, int argc, char *argv[]);

#endif /* SCRIPT_TEST_EVAL_H */
