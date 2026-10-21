/*
 * shell_ctx.h — Per-tab shell execution context
 *
 * Each tab maintains its own ShellContext with a virtual working directory (cwd),
 * exit status ($?), and a table of local variables.
 * In Thread mode, tabs share one process, so virtual CWD ensures that running
 * `cd` in one tab NEVER modifies another tab's directory.
 */
#ifndef EXEC_SHELL_CTX_H
#define EXEC_SHELL_CTX_H

#include "common.h"

#define MAX_VARS 128

typedef struct {
    char key[64];
    char val[MAX_CMD_LEN];
} ShellVar;

typedef struct {
    int      tab_id;
    int      in_fd;                 /* tab input pipe from GUI */
    int      out_fd;                /* tab output pipe to GUI */
    char     cwd[MAX_PATH_LEN];     /* virtual working directory */
    int      last_status;           /* exit status of last command ($?) */
    ShellVar vars[MAX_VARS];
    int      num_vars;
} ShellContext;

/*
 * Allocate and initialize a tab's ShellContext with current working directory and last_status=0.
 */
ShellContext *shell_ctx_create(int tab_id);

/*
 * Set or update a shell variable (NAME=value).
 */
void shell_ctx_set_var(ShellContext *ctx, const char *key, const char *val);

/*
 * Retrieve a shell variable value. Returns NULL if not found.
 */
const char *shell_ctx_get_var(ShellContext *ctx, const char *key);

/*
 * Update the tab's virtual CWD.
 */
void shell_ctx_set_cwd(ShellContext *ctx, const char *new_cwd);

/*
 * Clean up and free shell context resources.
 */
void shell_ctx_destroy(ShellContext *ctx);

#endif /* EXEC_SHELL_CTX_H */
