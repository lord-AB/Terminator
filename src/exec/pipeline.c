/*
 * pipeline.c — External command execution and output piping
 */
#include "exec/pipeline.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int pipeline_tokenize(char *line, char *argv[], int max_args) {
    if (!line || !argv || max_args <= 0) return 0;

    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (!*p) break;

        if (*p == '\'' || *p == '"') {
            /* Quoted token */
            char quote = *p++;
            argv[argc++] = p;
            while (*p && *p != quote) {
                p++;
            }
            if (*p == quote) {
                *p++ = '\0';
            }
        } else {
            /* Unquoted token */
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                p++;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
    }

    argv[argc] = NULL;
    return argc;
}

int pipeline_exec_single(ShellContext *ctx, char *argv[], int out_fd) {
    if (!ctx || !argv || !argv[0]) return 0;

    int pfd[2];
    if (pipe(pfd) < 0) {
        char err[128];
        snprintf(err, sizeof(err), "terminator: pipe failed: %s\n", strerror(errno));
        if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
        ctx->last_status = 1;
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        char err[128];
        snprintf(err, sizeof(err), "terminator: fork failed: %s\n", strerror(errno));
        if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
        close(pfd[0]);
        close(pfd[1]);
        ctx->last_status = 1;
        return 1;
    }

    if (pid == 0) {
        /* ── Child process ── */
        close(pfd[0]); /* close read end */

        /* Redirect stdout & stderr to pipe write end */
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);

        /* Switch to tab's virtual working directory */
        if (chdir(ctx->cwd) != 0) {
            fprintf(stderr, "terminator: cannot access directory %s: %s\n",
                    ctx->cwd, strerror(errno));
            fflush(stderr);
            _exit(1);
        }

        /* Execute command */
        execvp(argv[0], argv);

        /* If execvp fails */
        if (errno == ENOENT) {
            fprintf(stderr, "terminator: %s: command not found\n", argv[0]);
        } else {
            fprintf(stderr, "terminator: %s: %s\n", argv[0], strerror(errno));
        }
        fflush(stderr);
        _exit(127);
    }

    /* ── Parent process (tab worker) ── */
    close(pfd[1]); /* close write end */

    /* Stream output from child pipe to GUI */
    char buf[PIPE_BUF_SIZE];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (out_fd >= 0) {
            proto_send(out_fd, TAB_MSG_OUTPUT, buf, (uint32_t)n);
        }
    }
    close(pfd[0]);

    /* Wait for child to exit */
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        break;
    }

    if (WIFEXITED(status)) {
        ctx->last_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ctx->last_status = 128 + WTERMSIG(status);
    } else {
        ctx->last_status = 1;
    }

    return ctx->last_status;
}
