/*
 * redirect.c — Validates files and sets up redirected file descriptors
 */
#include "exec/redirect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int redirect_resolve_path(ShellContext *ctx, const char *path, char *resolved_path, size_t max_len) {
    if (!path || !resolved_path || max_len == 0) return -1;

    /* Absolute path */
    if (path[0] == '/') {
        strncpy(resolved_path, path, max_len - 1);
        resolved_path[max_len - 1] = '\0';
        return 0;
    }

    /* Home directory relative path (~ or ~/...) */
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "/";
        if (path[1] == '/' || path[1] == '\0') {
            snprintf(resolved_path, max_len, "%s%s", home, path + 1);
            return 0;
        }
    }

    /* Relative path: resolve relative to ctx->cwd */
    if (ctx && ctx->cwd[0] != '\0') {
        snprintf(resolved_path, max_len, "%s/%s", ctx->cwd, path);
    } else {
        strncpy(resolved_path, path, max_len - 1);
        resolved_path[max_len - 1] = '\0';
    }
    return 0;
}

int redirect_apply(ShellContext *ctx, const Command *cmd) {
    if (!cmd) return 0;

    /* 1. Input redirection (< infile) */
    if (cmd->infile && cmd->infile[0] != '\0') {
        char in_path[MAX_PATH_LEN];
        redirect_resolve_path(ctx, cmd->infile, in_path, sizeof(in_path));

        int in_fd = open(in_path, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", cmd->infile,
                    errno == ENOENT ? "No such file or directory" : strerror(errno));
            fflush(stderr);
            return -1;
        }

        if (dup2(in_fd, STDIN_FILENO) < 0) {
            fprintf(stderr, "terminator: dup2(stdin) failed: %s\n", strerror(errno));
            fflush(stderr);
            close(in_fd);
            return -1;
        }
        close(in_fd);
    }

    /* 2. Output redirection (> outfile or >> outfile) */
    if (cmd->outfile && cmd->outfile[0] != '\0') {
        char out_path[MAX_PATH_LEN];
        redirect_resolve_path(ctx, cmd->outfile, out_path, sizeof(out_path));

        int flags = O_WRONLY | O_CREAT | (cmd->append_out ? O_APPEND : O_TRUNC);
        int out_fd = open(out_path, flags, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "terminator: %s: %s\n", cmd->outfile, strerror(errno));
            fflush(stderr);
            return -1;
        }

        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            fprintf(stderr, "terminator: dup2(stdout) failed: %s\n", strerror(errno));
            fflush(stderr);
            close(out_fd);
            return -1;
        }
        close(out_fd);
    }

    return 0;
}
