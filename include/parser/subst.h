/*
 * subst.h — Innermost-first command substitution ($(cmd) and `cmd`)
 */
#ifndef PARSER_SUBST_H
#define PARSER_SUBST_H

#include <stddef.h>
#include "common.h"
#include "exec/shell_ctx.h"

/*
 * Expands command substitutions ($(cmd) and `cmd`) in `input`.
 * Rules:
 * - Single-quoted strings ('...') are strictly literal: no substitution.
 * - Double-quoted strings ("...") and unquoted text permit substitutions.
 * - Innermost substitutions run first (e.g. $(basename $(pwd))).
 * - Each subcommand runs in a child process with chdir(ctx->cwd).
 * - Child's stdout is captured, trailing newlines are stripped, and output
 *   is spliced back into the command line.
 *
 * Output is written to `output` (up to `out_size` bytes).
 * Returns 0 on success, -1 on buffer overflow or execution error.
 */
int subst_expand(ShellContext *ctx, const char *input, char *output, size_t out_size);

#endif /* PARSER_SUBST_H */
