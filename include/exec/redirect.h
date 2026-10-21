/*
 * redirect.h — I/O redirection file descriptor setup using open() and dup2()
 */
#ifndef EXEC_REDIRECT_H
#define EXEC_REDIRECT_H

#include <stddef.h>
#include "common.h"
#include "exec/shell_ctx.h"
#include "parser/ast.h"

/*
 * Resolves a file path relative to ctx->cwd.
 * - Absolute paths (/...) are preserved.
 * - Home-relative paths (~/...) are expanded using $HOME.
 * - Relative paths are prepended with ctx->cwd.
 *
 * Populates `resolved_path` up to `max_len`.
 * Returns 0 on success, -1 on error.
 */
int redirect_resolve_path(ShellContext *ctx, const char *path, char *resolved_path, size_t max_len);

/*
 * Sets up file descriptors for input and output redirections in child process.
 * - If cmd->infile is specified:
 *     Opens file O_RDONLY.
 *     If open fails: prints "terminator: <infile>: No such file or directory" to stderr
 *     and returns -1.
 *     Otherwise calls dup2(fd, STDIN_FILENO) and close(fd).
 * - If cmd->outfile is specified:
 *     Opens file with O_WRONLY | O_CREAT | (O_TRUNC or O_APPEND), mode 0644.
 *     If open fails: prints error to stderr and returns -1.
 *     Otherwise calls dup2(fd, STDOUT_FILENO) and close(fd).
 *
 * Returns 0 on success, -1 on error.
 */
int redirect_apply(ShellContext *ctx, const Command *cmd);

#endif /* EXEC_REDIRECT_H */
