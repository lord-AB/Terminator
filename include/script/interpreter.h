/*
 * interpreter.h — Line-by-line bash script interpreter
 */
#ifndef SCRIPT_INTERPRETER_H
#define SCRIPT_INTERPRETER_H

#include "exec/shell_ctx.h"

/*
 * Checks if a command or file is a bash script:
 * - Starts with "./" or "/" and file exists and has shebang "#!/bin/bash" or ".sh" extension
 * - Or filename ends in ".sh" and exists in ctx->cwd
 * Returns 1 if script, 0 otherwise.
 */
int interpreter_is_script(ShellContext *ctx, const char *cmd);

/*
 * Executes a bash script file.
 * - filepath: path to script (relative to ctx->cwd or absolute)
 * - argc, argv: arguments ($0 is script name, $1..$# are args)
 * - is_source: 1 if executed via `source` (in current ctx), 0 if executed as child script (`bash` or `./`)
 * - out_fd: pipe/file descriptor to stream output to
 * Returns script exit status ($?).
 */
int interpreter_run_file(ShellContext *ctx, const char *filepath, int argc, char *argv[], int is_source, int out_fd);

/*
 * Executes a single command/pipeline line within the interpreter execution context.
 * Handles substitutions, expansions, AST parsing, built-ins, and pipelines.
 * Returns command exit status ($?).
 */
int interpreter_exec_line(ShellContext *ctx, const char *line, int out_fd);

#endif /* SCRIPT_INTERPRETER_H */
