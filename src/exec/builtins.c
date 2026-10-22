/*
 * builtins.c — Implementation of shell built-in commands
 */
#include "exec/builtins.h"
#include "exec/redirect.h"
#include "exec/jobs.h"
#include "features/multiwatch.h"
#include "script/interpreter.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

/* Helper to send text output to the GUI via pipe */
static void send_output(int out_fd, const char *msg) {
    if (out_fd >= 0 && msg) {
        proto_send(out_fd, TAB_MSG_OUTPUT, msg, (uint32_t)strlen(msg));
    }
}

int builtins_is_builtin(const char *cmd) {
    if (!cmd) return 0;
    if (strcmp(cmd, "cd") == 0 ||
        strcmp(cmd, "exit") == 0 ||
        strcmp(cmd, "history") == 0 ||
        strcmp(cmd, "multiWatch") == 0 ||
        strcmp(cmd, "jobs") == 0 ||
        strcmp(cmd, "fg") == 0 ||
        strcmp(cmd, "bg") == 0 ||
        strcmp(cmd, "source") == 0 ||
        strcmp(cmd, ".") == 0) {
        return 1;
    }
    return 0;
}

int builtin_cd(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    const char *target = NULL;
    char target_path[MAX_PATH_LEN];

    if (argc < 2 || strcmp(argv[1], "~") == 0) {
        /* Default to HOME */
        target = getenv("HOME");
        if (!target) target = "/";
    } else if (strcmp(argv[1], "-") == 0) {
        /* OLDPWD */
        const char *oldpwd = shell_ctx_get_var(ctx, "OLDPWD");
        if (!oldpwd) {
            char err[] = "terminator: cd: OLDPWD not set\n";
            send_output(out_fd, err);
            ctx->last_status = BUILTIN_ERROR;
            return BUILTIN_ERROR;
        }
        target = oldpwd;
        /* Print directory like bash does when cd - */
        char msg[MAX_PATH_LEN + 2];
        snprintf(msg, sizeof(msg), "%s\n", oldpwd);
        send_output(out_fd, msg);
    } else if (strncmp(argv[1], "~/", 2) == 0) {
        const char *home = getenv("HOME");
        if (!home) home = "/";
        snprintf(target_path, sizeof(target_path), "%s/%s", home, argv[1] + 2);
        target = target_path;
    } else {
        target = argv[1];
    }

    /* Form absolute path to resolve cleanly */
    char full_path[MAX_PATH_LEN];
    if (target[0] == '/') {
        strncpy(full_path, target, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", ctx->cwd, target);
    }

    /* Canonicalize path using realpath without changing process working directory */
    char resolved[MAX_PATH_LEN];
    if (!realpath(full_path, resolved)) {
        char err[MAX_PATH_LEN + 64];
        snprintf(err, sizeof(err), "terminator: cd: %s: %s\n", argv[1], strerror(errno));
        send_output(out_fd, err);
        ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    /* Verify it is a directory */
    struct stat st;
    if (stat(resolved, &st) != 0) {
        char err[MAX_PATH_LEN + 64];
        snprintf(err, sizeof(err), "terminator: cd: %s: %s\n", argv[1], strerror(errno));
        send_output(out_fd, err);
        ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    if (!S_ISDIR(st.st_mode)) {
        char err[MAX_PATH_LEN + 64];
        snprintf(err, sizeof(err), "terminator: cd: %s: Not a directory\n", argv[1]);
        send_output(out_fd, err);
        ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    /* Check access permissions */
    if (access(resolved, X_OK) != 0) {
        char err[MAX_PATH_LEN + 64];
        snprintf(err, sizeof(err), "terminator: cd: %s: Permission denied\n", argv[1]);
        send_output(out_fd, err);
        ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    /* Save current directory to OLDPWD */
    shell_ctx_set_var(ctx, "OLDPWD", ctx->cwd);

    /* Update virtual CWD */
    shell_ctx_set_cwd(ctx, resolved);
    ctx->last_status = BUILTIN_SUCCESS;

    /* Notify GUI about CWD change to update tab title */
    if (out_fd >= 0) {
        proto_send(out_fd, TAB_MSG_CWD, resolved, (uint32_t)strlen(resolved));
    }

    return BUILTIN_SUCCESS;
}

int builtin_exit(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    int exit_code = ctx->last_status;
    if (argc >= 2) {
        exit_code = atoi(argv[1]);
    }
    ctx->last_status = exit_code;

    /* Send exit notification to GUI */
    if (out_fd >= 0) {
        proto_send(out_fd, TAB_MSG_EXIT, &exit_code, sizeof(exit_code));
    }

    return BUILTIN_EXIT;
}

int builtin_history(ShellContext *ctx, int argc, char *argv[], int out_fd, SharedState *shared) {
    (void)argc;
    (void)argv;
    if (!shared) {
        send_output(out_fd, "history: shared memory not available\n");
        ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    int total = shm_history_count(shared);
    int start = (total > HISTORY_DISPLAY) ? (total - HISTORY_DISPLAY) : 0;

    for (int i = start; i < total; i++) {
        const char *cmd = shm_history_get(shared, i);
        if (cmd) {
            char line[MAX_CMD_LEN + 32];
            snprintf(line, sizeof(line), "%5d  %s\n", i + 1, cmd);
            send_output(out_fd, line);
        }
    }

    ctx->last_status = BUILTIN_SUCCESS;
    return BUILTIN_SUCCESS;
}

int builtin_multiwatch(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    char *cmds[MULTIWATCH_MAX_CMDS];
    int count = multiwatch_parse_args(argc, argv, cmds, MULTIWATCH_MAX_CMDS);
    if (count <= 0) {
        char err[] = "terminator: multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n";
        send_output(out_fd, err);
        if (ctx) ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    int in_fd = ctx ? ctx->in_fd : -1;
    int res = multiwatch_run(ctx, count, cmds, out_fd, in_fd);
    multiwatch_free_cmds(cmds, count);
    return res;
}

int builtin_jobs(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    (void)argc;
    (void)argv;
    if (!ctx || !ctx->jobs) return BUILTIN_SUCCESS;
    jobs_list(ctx->jobs, out_fd);
    ctx->last_status = BUILTIN_SUCCESS;
    return BUILTIN_SUCCESS;
}

int builtin_fg(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    if (!ctx || !ctx->jobs) return BUILTIN_ERROR;
    int job_id = 0;
    if (argc >= 2 && argv[1]) {
        const char *p = argv[1];
        if (*p == '%') p++;
        job_id = atoi(p);
    }
    int in_fd = ctx ? ctx->in_fd : -1;
    int res = jobs_fg(ctx, ctx->jobs, job_id, out_fd, in_fd);
    ctx->last_status = res;
    return res;
}

int builtin_bg(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    if (!ctx || !ctx->jobs) return BUILTIN_ERROR;
    int job_id = 0;
    if (argc >= 2 && argv[1]) {
        const char *p = argv[1];
        if (*p == '%') p++;
        job_id = atoi(p);
    }
    int res = jobs_bg(ctx->jobs, job_id, out_fd);
    ctx->last_status = res;
    return res;
}

int builtin_source(ShellContext *ctx, int argc, char *argv[], int out_fd) {
    if (argc < 2 || !argv[1]) {
        send_output(out_fd, "terminator: source: filename argument required\n");
        ctx->last_status = 1;
        return 1;
    }
    int res = interpreter_run_file(ctx, argv[1], argc - 1, &argv[1], 1 /* is_source */, out_fd);
    ctx->last_status = res;
    return res;
}

int builtins_exec(ShellContext *ctx, int argc, char *argv[], int out_fd, SharedState *shared) {
    if (argc == 0 || !argv[0]) return BUILTIN_SUCCESS;

    if (strcmp(argv[0], "cd") == 0) {
        return builtin_cd(ctx, argc, argv, out_fd);
    }
    if (strcmp(argv[0], "exit") == 0) {
        return builtin_exit(ctx, argc, argv, out_fd);
    }
    if (strcmp(argv[0], "history") == 0) {
        return builtin_history(ctx, argc, argv, out_fd, shared);
    }
    if (strcmp(argv[0], "multiWatch") == 0) {
        return builtin_multiwatch(ctx, argc, argv, out_fd);
    }
    if (strcmp(argv[0], "jobs") == 0) {
        return builtin_jobs(ctx, argc, argv, out_fd);
    }
    if (strcmp(argv[0], "fg") == 0) {
        return builtin_fg(ctx, argc, argv, out_fd);
    }
    if (strcmp(argv[0], "bg") == 0) {
        return builtin_bg(ctx, argc, argv, out_fd);
    }
    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        return builtin_source(ctx, argc, argv, out_fd);
    }

    return BUILTIN_ERROR;
}

int builtins_exec_cmd(ShellContext *ctx, Command *cmd, int out_fd, SharedState *shared) {
    if (!ctx || !cmd || cmd->argc == 0 || !cmd->argv[0]) return BUILTIN_SUCCESS;

    /* If no output redirection, execute normally */
    if (!cmd->outfile || cmd->outfile[0] == '\0') {
        return builtins_exec(ctx, cmd->argc, cmd->argv, out_fd, shared);
    }

    /* Output redirection for built-in */
    char out_path[MAX_PATH_LEN];
    redirect_resolve_path(ctx, cmd->outfile, out_path, sizeof(out_path));
    int flags = O_WRONLY | O_CREAT | (cmd->append_out ? O_APPEND : O_TRUNC);
    int file_fd = open(out_path, flags, 0644);
    if (file_fd < 0) {
        char err[MAX_CMD_LEN];
        snprintf(err, sizeof(err), "terminator: %s: %s\n", cmd->outfile, strerror(errno));
        send_output(out_fd, err);
        ctx->last_status = BUILTIN_ERROR;
        return BUILTIN_ERROR;
    }

    int pfd[2];
    if (pipe(pfd) < 0) {
        close(file_fd);
        return builtins_exec(ctx, cmd->argc, cmd->argv, out_fd, shared);
    }

    int res = builtins_exec(ctx, cmd->argc, cmd->argv, pfd[1], shared);
    close(pfd[1]);

    uint32_t type, len;
    char buf[PIPE_BUF_SIZE];
    while (proto_recv(pfd[0], &type, buf, sizeof(buf), &len) == 0) {
        if (type == TAB_MSG_OUTPUT && len > 0) {
            ssize_t w = write(file_fd, buf, len);
            (void)w;
        } else if (type != TAB_MSG_OUTPUT) {
            proto_send(out_fd, type, buf, len);
        }
    }
    close(pfd[0]);
    close(file_fd);

    return res;
}
