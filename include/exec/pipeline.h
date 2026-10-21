/*
 * pipeline.h — Command execution and pipe orchestration
 */
#ifndef EXEC_PIPELINE_H
#define EXEC_PIPELINE_H

#include "common.h"
#include "exec/shell_ctx.h"

/*
 * Tokenizes a line into an argv array.
 * Respects single and double quotes. Modifies the buffer in-place.
 * Returns the number of tokens (argc). argv[argc] is set to NULL.
 */
int pipeline_tokenize(char *line, char *argv[], int max_args);

/*
 * Executes a single external command via fork() + execvp().
 * Child changes directory to ctx->cwd before execvp.
 * Output (stdout & stderr) is streamed to out_fd using TAB_MSG_OUTPUT.
 * Waits for child process and updates ctx->last_status ($?).
 * Returns the child's exit status.
 */
int pipeline_exec_single(ShellContext *ctx, char *argv[], int out_fd);

#endif /* EXEC_PIPELINE_H */
