/*
 * subst.c — Command substitution with innermost-first evaluation
 */
#include "parser/subst.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAX_SUBST_DEPTH 64

typedef struct {
    int is_backtick;     /* 1 for `...`, 0 for $(...) */
    size_t start_idx;    /* index of '$' or '`' */
    size_t cmd_start;    /* index of first command character */
} SubstFrame;

/*
 * Finds the innermost command substitution in `str`.
 * Returns 1 if found (with offsets set), or 0 if none found.
 */
static int find_innermost(const char *str, size_t *sub_start, size_t *cmd_start,
                          size_t *cmd_len, size_t *sub_end) {
    SubstFrame stack[MAX_SUBST_DEPTH];
    int depth = 0;
    int in_squote = 0;
    int in_dquote = 0;

    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];

        /* Escaped character outside single quotes */
        if (c == '\\' && !in_squote) {
            if (str[i + 1] != '\0') {
                i++;
                continue;
            }
        }

        /* Single quote handling: literal, substitutions disabled */
        if (c == '\'' && !in_dquote) {
            in_squote = !in_squote;
            continue;
        }
        if (in_squote) {
            continue;
        }

        /* Double quote handling: substitutions allowed */
        if (c == '"' && depth == 0) {
            in_dquote = !in_dquote;
            continue;
        }

        /* Check for $( */
        if (c == '$' && str[i + 1] == '(') {
            if (depth < MAX_SUBST_DEPTH) {
                stack[depth].is_backtick = 0;
                stack[depth].start_idx = i;
                stack[depth].cmd_start = i + 2;
                depth++;
            }
            i++; /* skip '(' */
            continue;
        }

        /* Check for ` */
        if (c == '`') {
            if (depth > 0 && stack[depth - 1].is_backtick) {
                /* Matching backtick found! This is innermost. */
                depth--;
                *sub_start = stack[depth].start_idx;
                *cmd_start = stack[depth].cmd_start;
                *cmd_len   = i - stack[depth].cmd_start;
                *sub_end   = i + 1;
                return 1;
            } else {
                /* Opening backtick */
                if (depth < MAX_SUBST_DEPTH) {
                    stack[depth].is_backtick = 1;
                    stack[depth].start_idx = i;
                    stack[depth].cmd_start = i + 1;
                    depth++;
                }
                continue;
            }
        }

        /* Check for ) closing $( */
        if (c == ')' && depth > 0 && !stack[depth - 1].is_backtick) {
            depth--;
            *sub_start = stack[depth].start_idx;
            *cmd_start = stack[depth].cmd_start;
            *cmd_len   = i - stack[depth].cmd_start;
            *sub_end   = i + 1;
            return 1;
        }
    }

    return 0;
}

/*
 * Runs `cmd` in a child process, capturing stdout via pipe.
 * Strips trailing newlines from captured output.
 */
static int run_subcommand(ShellContext *ctx, const char *cmd, char *out_buf, size_t out_max) {
    int pfd[2];
    if (pipe(pfd) < 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);

        /* Redirect stderr to /dev/null to avoid mixing error text into command output */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        if (ctx && ctx->cwd[0] != '\0') {
            chdir(ctx->cwd);
        }

        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    /* Parent */
    close(pfd[1]);

    size_t total_read = 0;
    while (total_read < out_max - 1) {
        ssize_t n = read(pfd[0], out_buf + total_read, out_max - 1 - total_read);
        if (n <= 0) break;
        total_read += (size_t)n;
    }
    out_buf[total_read] = '\0';
    close(pfd[0]);

    int status;
    waitpid(pid, &status, 0);

    /* Strip trailing newlines */
    while (total_read > 0 && (out_buf[total_read - 1] == '\n' || out_buf[total_read - 1] == '\r')) {
        out_buf[--total_read] = '\0';
    }

    return 0;
}

int subst_expand(ShellContext *ctx, const char *input, char *output, size_t out_size) {
    if (!input || !output || out_size == 0) return -1;

    char current[MAX_CMD_LEN];
    strncpy(current, input, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    int iterations = 0;
    size_t sub_start, cmd_start, cmd_len, sub_end;

    while (find_innermost(current, &sub_start, &cmd_start, &cmd_len, &sub_end)) {
        if (++iterations > MAX_SUBST_DEPTH) {
            /* Protect against infinite recursion */
            break;
        }

        /* Extract command string */
        char cmd[MAX_CMD_LEN];
        if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
        memcpy(cmd, current + cmd_start, cmd_len);
        cmd[cmd_len] = '\0';

        /* Execute command and capture output */
        char sub_result[MAX_CMD_LEN];
        sub_result[0] = '\0';
        run_subcommand(ctx, cmd, sub_result, sizeof(sub_result));

        /* Splice result into current string: prefix + sub_result + suffix */
        char next[MAX_CMD_LEN];
        int written = snprintf(next, sizeof(next), "%.*s%s%s",
                               (int)sub_start, current, sub_result, current + sub_end);
        if (written < 0 || (size_t)written >= sizeof(next)) {
            return -1; /* Overflow */
        }

        strncpy(current, next, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';
    }

    if (strlen(current) >= out_size) {
        return -1;
    }
    strncpy(output, current, out_size - 1);
    output[out_size - 1] = '\0';
    return 0;
}
