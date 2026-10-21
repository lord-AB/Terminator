/*
 * shell_ctx.c — Implementation of per-tab shell execution context
 */
#include "exec/shell_ctx.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

ShellContext *shell_ctx_create(int tab_id) {
    ShellContext *ctx = calloc(1, sizeof(ShellContext));
    if (!ctx) return NULL;

    ctx->tab_id = tab_id;
    ctx->last_status = 0;
    ctx->num_vars = 0;

    /* Initialize virtual working directory to current process directory */
    if (!getcwd(ctx->cwd, sizeof(ctx->cwd))) {
        strncpy(ctx->cwd, "/", sizeof(ctx->cwd) - 1);
        ctx->cwd[sizeof(ctx->cwd) - 1] = '\0';
    }

    return ctx;
}

void shell_ctx_set_var(ShellContext *ctx, const char *key, const char *val) {
    if (!ctx || !key || !val) return;

    /* Check if variable already exists */
    for (int i = 0; i < ctx->num_vars; i++) {
        if (strcmp(ctx->vars[i].key, key) == 0) {
            strncpy(ctx->vars[i].val, val, sizeof(ctx->vars[i].val) - 1);
            ctx->vars[i].val[sizeof(ctx->vars[i].val) - 1] = '\0';
            return;
        }
    }

    /* Add new variable if slot available */
    if (ctx->num_vars < MAX_VARS) {
        strncpy(ctx->vars[ctx->num_vars].key, key, sizeof(ctx->vars[ctx->num_vars].key) - 1);
        ctx->vars[ctx->num_vars].key[sizeof(ctx->vars[ctx->num_vars].key) - 1] = '\0';

        strncpy(ctx->vars[ctx->num_vars].val, val, sizeof(ctx->vars[ctx->num_vars].val) - 1);
        ctx->vars[ctx->num_vars].val[sizeof(ctx->vars[ctx->num_vars].val) - 1] = '\0';

        ctx->num_vars++;
    }
}

const char *shell_ctx_get_var(ShellContext *ctx, const char *key) {
    if (!ctx || !key) return NULL;
    for (int i = 0; i < ctx->num_vars; i++) {
        if (strcmp(ctx->vars[i].key, key) == 0) {
            return ctx->vars[i].val;
        }
    }
    return NULL;
}

void shell_ctx_set_cwd(ShellContext *ctx, const char *new_cwd) {
    if (!ctx || !new_cwd) return;
    strncpy(ctx->cwd, new_cwd, sizeof(ctx->cwd) - 1);
    ctx->cwd[sizeof(ctx->cwd) - 1] = '\0';
}

void shell_ctx_destroy(ShellContext *ctx) {
    if (!ctx) return;
    free(ctx);
}
