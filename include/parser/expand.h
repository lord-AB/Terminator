/*
 * expand.h — Variable and parameter expansion for Terminator
 */
#ifndef PARSER_EXPAND_H
#define PARSER_EXPAND_H

#include <stddef.h>
#include "common.h"
#include "exec/shell_ctx.h"

/*
 * Expands variables ($VAR, $?, ${VAR}) in `input`.
 * Rules:
 * - Single-quoted strings ('...') are strictly literal: no expansion.
 * - Double-quoted strings ("...") expand $VAR, $?, and ${VAR}.
 * - Unquoted text expands $VAR, $?, and ${VAR}.
 * - Line continuations (\\ followed by \n) are removed.
 *
 * Output is written to `output` (up to `out_size` bytes).
 * Returns 0 on success, -1 on buffer overflow.
 */
int expand_variables(ShellContext *ctx, const char *input, char *output, size_t out_size);

#endif /* PARSER_EXPAND_H */
