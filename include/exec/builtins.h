/*
 * builtins.h — Shell built-in commands (cd, exit, history, multiWatch, jobs, fg, bg, source)
 */
#ifndef EXEC_BUILTINS_H
#define EXEC_BUILTINS_H

#include "common.h"
#include "exec/shell_ctx.h"
#include "ipc/shm_sync.h"

#define BUILTIN_SUCCESS      0
#define BUILTIN_ERROR        1
#define BUILTIN_EXIT       100

/*
 * Returns 1 if `cmd` is a recognized built-in name, 0 otherwise.
 */
int builtins_is_builtin(const char *cmd);

/*
 * Dispatches and executes a built-in command.
 * Output is sent to out_fd using proto_send(..., TAB_MSG_OUTPUT, ...).
 * If the command is `exit`, returns BUILTIN_EXIT.
 * Otherwise returns 0 on success or non-zero error code.
 */
int builtins_exec(ShellContext *ctx, int argc, char *argv[], int out_fd, SharedState *shared);

/* Specific built-in implementations */
int builtin_cd(ShellContext *ctx, int argc, char *argv[], int out_fd);
int builtin_exit(ShellContext *ctx, int argc, char *argv[], int out_fd);
int builtin_history(ShellContext *ctx, int argc, char *argv[], int out_fd, SharedState *shared);
int builtin_multiwatch(ShellContext *ctx, int argc, char *argv[], int out_fd);
int builtin_jobs(ShellContext *ctx, int argc, char *argv[], int out_fd);
int builtin_fg(ShellContext *ctx, int argc, char *argv[], int out_fd);
int builtin_bg(ShellContext *ctx, int argc, char *argv[], int out_fd);
int builtin_source(ShellContext *ctx, int argc, char *argv[], int out_fd);

#endif /* EXEC_BUILTINS_H */
