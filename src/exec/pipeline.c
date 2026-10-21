/*
 * pipeline.c — External command and pipeline execution with pipe orchestration
 */
#include "exec/pipeline.h"
#include "exec/redirect.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <glob.h>

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

/*
 * Expands wildcards (*, ?, [) in command arguments (invoked inside child after chdir).
 */
static void expand_globs(Command *cmd) {
    if (!cmd || cmd->argc <= 0) return;

    char *new_argv[MAX_ARGS];
    int new_argc = 0;

    for (int i = 0; i < cmd->argc; i++) {
        const char *arg = cmd->argv[i];
        if (arg && (strchr(arg, '*') || strchr(arg, '?') || strchr(arg, '['))) {
            glob_t g;
            memset(&g, 0, sizeof(g));
            if (glob(arg, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
                for (size_t j = 0; j < g.gl_pathc && new_argc < MAX_ARGS - 1; j++) {
                    new_argv[new_argc++] = strdup(g.gl_pathv[j]);
                }
                globfree(&g);
                continue;
            }
        }
        if (new_argc < MAX_ARGS - 1) {
            new_argv[new_argc++] = strdup(arg ? arg : "");
        }
    }
    new_argv[new_argc] = NULL;

    for (int i = 0; i < new_argc; i++) {
        cmd->argv[i] = new_argv[i];
    }
    cmd->argv[new_argc] = NULL;
    cmd->argc = new_argc;
}

int pipeline_exec_cmd(ShellContext *ctx, Command *cmd, int out_fd) {
    if (!ctx || !cmd) return 0;

    /* Handle redirection-only invocation without a command (e.g. "< missing_file.txt") */
    if (cmd->argc == 0 || !cmd->argv[0]) {
        if (cmd->infile && cmd->infile[0] != '\0') {
            char in_path[MAX_PATH_LEN];
            redirect_resolve_path(ctx, cmd->infile, in_path, sizeof(in_path));
            int in_fd = open(in_path, O_RDONLY);
            if (in_fd < 0) {
                char err[MAX_CMD_LEN];
                snprintf(err, sizeof(err), "terminator: %s: %s\n", cmd->infile,
                         errno == ENOENT ? "No such file or directory" : strerror(errno));
                if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
                ctx->last_status = 1;
                return 1;
            }
            close(in_fd);
        }
        if (cmd->outfile && cmd->outfile[0] != '\0') {
            char out_path[MAX_PATH_LEN];
            redirect_resolve_path(ctx, cmd->outfile, out_path, sizeof(out_path));
            int flags = O_WRONLY | O_CREAT | (cmd->append_out ? O_APPEND : O_TRUNC);
            int fd = open(out_path, flags, 0644);
            if (fd < 0) {
                char err[MAX_CMD_LEN];
                snprintf(err, sizeof(err), "terminator: %s: %s\n", cmd->outfile, strerror(errno));
                if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
                ctx->last_status = 1;
                return 1;
            }
            close(fd);
        }
        ctx->last_status = 0;
        return 0;
    }

    /* Wrap single command in a 1-command pipeline */
    Pipeline pl;
    pipeline_init(&pl);
    pl.commands[0] = *cmd;
    pl.num_commands = 1;
    return pipeline_exec_pipeline(ctx, &pl, out_fd);
}

int pipeline_exec_pipeline(ShellContext *ctx, Pipeline *pl, int out_fd) {
    if (!ctx || !pl || pl->num_commands <= 0) return 0;

    int N = pl->num_commands;

    /* Allocate N - 1 inter-command pipes */
    int pipes[MAX_COMMANDS_IN_PIPELINE][2];
    for (int i = 0; i < N - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            char err[128];
            snprintf(err, sizeof(err), "terminator: pipe failed: %s\n", strerror(errno));
            if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            ctx->last_status = 1;
            return 1;
        }
    }

    /* Final capture pipe for streaming the pipeline output to the GUI */
    int capture_pipe[2];
    if (pipe(capture_pipe) < 0) {
        char err[128];
        snprintf(err, sizeof(err), "terminator: pipe failed: %s\n", strerror(errno));
        if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
        for (int j = 0; j < N - 1; j++) {
            close(pipes[j][0]);
            close(pipes[j][1]);
        }
        ctx->last_status = 1;
        return 1;
    }

    /* Fork N children */
    pid_t pids[MAX_COMMANDS_IN_PIPELINE];
    for (int i = 0; i < N; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            char err[128];
            snprintf(err, sizeof(err), "terminator: fork failed: %s\n", strerror(errno));
            if (out_fd >= 0) proto_send(out_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
            for (int j = 0; j < N - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);
            ctx->last_status = 1;
            return 1;
        }

        if (pids[i] == 0) {
            /* ── Child process i ── */

            /* 1. Wire stdin: if i > 0, read from previous pipe */
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }

            /* 2. Wire stdout: if i < N - 1, write to next pipe; else write to capture pipe */
            if (i < N - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            } else {
                dup2(capture_pipe[1], STDOUT_FILENO);
            }

            /* 3. Wire stderr to capture pipe so error messages appear in terminal */
            dup2(capture_pipe[1], STDERR_FILENO);

            /* 4. Switch working directory to tab's virtual directory */
            if (chdir(ctx->cwd) != 0) {
                fprintf(stderr, "terminator: cannot access directory %s: %s\n",
                        ctx->cwd, strerror(errno));
                fflush(stderr);
                _exit(1);
            }

            /* 5. Apply explicit redirections (< infile, > outfile, >> outfile) AFTER wiring pipes */
            if (redirect_apply(ctx, &pl->commands[i]) < 0) {
                _exit(1);
            }

            /* 6. Close all pipe FDs in child */
            for (int j = 0; j < N - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(capture_pipe[0]);
            close(capture_pipe[1]);

            /* If command segment has no argv (e.g. only redirection) */
            if (pl->commands[i].argc == 0 || !pl->commands[i].argv[0]) {
                _exit(0);
            }

            /* 7. Expand wildcards in child after chdir */
            expand_globs(&pl->commands[i]);

            /* 8. Execute command */
            execvp(pl->commands[i].argv[0], pl->commands[i].argv);

            /* If execvp fails */
            if (errno == ENOENT) {
                fprintf(stderr, "terminator: %s: command not found\n", pl->commands[i].argv[0]);
            } else {
                fprintf(stderr, "terminator: %s: %s\n", pl->commands[i].argv[0], strerror(errno));
            }
            fflush(stderr);
            _exit(127);
        }
    }

    /* ── Parent process (tab worker) ── */

    /* 1. Close all inter-command pipes in parent so EOF propagates properly */
    for (int j = 0; j < N - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }

    /* 2. Close capture pipe write end in parent so reader gets EOF when children terminate */
    close(capture_pipe[1]);

    /* 3. Stream output from capture_pipe[0] to out_fd (GUI) */
    char buf[PIPE_BUF_SIZE];
    ssize_t n;
    while ((n = read(capture_pipe[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        if (out_fd >= 0) {
            proto_send(out_fd, TAB_MSG_OUTPUT, buf, (uint32_t)n);
        }
    }
    close(capture_pipe[0]);

    /* 4. Wait for all N children */
    int last_status = 0;
    for (int i = 0; i < N; i++) {
        int status = 0;
        while (waitpid(pids[i], &status, 0) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Bash pipeline exit status rule: $? is the exit code of the last command */
        if (i == N - 1) {
            if (WIFEXITED(status)) {
                last_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                last_status = 128 + WTERMSIG(status);
            } else {
                last_status = 1;
            }
        }
    }

    ctx->last_status = last_status;
    return last_status;
}

int pipeline_exec_single(ShellContext *ctx, char *argv[], int out_fd) {
    if (!ctx || !argv || !argv[0]) return 0;
    Command cmd;
    command_init(&cmd);
    while (argv[cmd.argc] && cmd.argc < MAX_ARGS - 1) {
        cmd.argv[cmd.argc] = argv[cmd.argc];
        cmd.argc++;
    }
    cmd.argv[cmd.argc] = NULL;
    return pipeline_exec_cmd(ctx, &cmd, out_fd);
}
