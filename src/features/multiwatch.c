/*
 * multiwatch.c — Implementation of multiWatch parallel command execution
 */
#include "features/multiwatch.h"
#include "exec/pipeline.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <ctype.h>

static volatile sig_atomic_t g_multiwatch_interrupted = 0;

static void multiwatch_sigint_handler(int sig) {
    (void)sig;
    g_multiwatch_interrupted = 1;
}

void multiwatch_free_cmds(char *cmds[], int count) {
    if (!cmds) return;
    for (int i = 0; i < count; i++) {
        if (cmds[i]) {
            free(cmds[i]);
            cmds[i] = NULL;
        }
    }
}

int multiwatch_parse_args(int argc, char *argv[], char *cmds[], int max_cmds) {
    if (argc < 2 || !argv || !cmds || max_cmds <= 0) return 0;

    int count = 0;

    /*
     * Case 1: Single argument containing the entire bracketed list
     * e.g., argv[1] = "[\"date +%T\", \"ls | wc -l\"]"
     */
    if (argc == 2 && argv[1][0] == '[') {
        const char *p = argv[1] + 1;
        while (*p && count < max_cmds) {
            while (*p && (*p == ' ' || *p == '\t' || *p == ',')) p++;
            if (*p == ']' || *p == '\0') break;

            if (*p == '"' || *p == '\'') {
                char quote = *p++;
                const char *start = p;
                while (*p && *p != quote) {
                    if (*p == '\\' && *(p + 1)) p += 2;
                    else p++;
                }
                size_t len = p - start;
                char *cmd = malloc(len + 1);
                if (cmd) {
                    memcpy(cmd, start, len);
                    cmd[len] = '\0';
                    cmds[count++] = cmd;
                }
                if (*p == quote) p++;
            } else {
                const char *start = p;
                while (*p && *p != ',' && *p != ']' && *p != '\n') p++;
                const char *end = p;
                while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
                size_t len = end - start;
                if (len > 0) {
                    char *cmd = malloc(len + 1);
                    if (cmd) {
                        memcpy(cmd, start, len);
                        cmd[len] = '\0';
                        cmds[count++] = cmd;
                    }
                }
            }
        }
        return count;
    }

    /*
     * Case 2: Tokenized arguments from lexer
     * e.g., argv[1] = "[date +%T,", argv[2] = "ls | wc -l]"
     * or argv[1] = "[", argv[2] = "date +%T", argv[3] = ",", argv[4] = "ls | wc -l", argv[5] = "]"
     */
    for (int i = 1; i < argc && count < max_cmds; i++) {
        const char *p = argv[i];
        while (*p && (*p == '[' || isspace((unsigned char)*p))) p++;

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == ']' || p[len - 1] == ',' || isspace((unsigned char)p[len - 1]))) {
            len--;
        }

        /* Strip outer quotes if still present */
        if (len >= 2 && ((p[0] == '"' && p[len - 1] == '"') || (p[0] == '\'' && p[len - 1] == '\''))) {
            p++;
            len -= 2;
            while (*p && isspace((unsigned char)*p)) { p++; len--; }
            while (len > 0 && isspace((unsigned char)p[len - 1])) len--;
        }

        if (len > 0) {
            char *cmd = malloc(len + 1);
            if (cmd) {
                memcpy(cmd, p, len);
                cmd[len] = '\0';
                cmds[count++] = cmd;
            }
        }
    }

    return count;
}

static int spawn_command(ShellContext *ctx, MultiwatchCommand *mc) {
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
        /* Child: create new process group */
        setpgid(0, 0);

        /* Redirect stdout and stderr to pipe */
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);

        /* Switch to tab's virtual CWD */
        if (ctx && ctx->cwd[0]) {
            if (chdir(ctx->cwd) < 0) {
                /* proceed even if chdir fails */
            }
        }

        /* Execute command natively via Terminator's pipeline engine without /bin/sh */
        pipeline_exec_string_raw(ctx, mc->cmd);
        _exit(127);
    }

    /* Parent */
    close(pfd[1]);
    setpgid(pid, pid);

    mc->pid = pid;
    mc->pipe_fd = pfd[0];
    mc->running = 1;
    return 0;
}

int multiwatch_run(ShellContext *ctx, int num_cmds, char *cmds[], int out_fd, int in_fd) {
    if (num_cmds <= 0 || !cmds) {
        const char usage[] = "terminator: multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n";
        if (out_fd >= 0) {
            proto_send(out_fd, TAB_MSG_OUTPUT, usage, (uint32_t)strlen(usage));
        }
        if (ctx) ctx->last_status = 1;
        return 1;
    }

    /* Setup SIGINT signal handler */
    g_multiwatch_interrupted = 0;
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = multiwatch_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, &old_sa);

    MultiwatchCommand cmds_state[MULTIWATCH_MAX_CMDS];
    memset(cmds_state, 0, sizeof(cmds_state));

    int total_cmds = (num_cmds > MULTIWATCH_MAX_CMDS) ? MULTIWATCH_MAX_CMDS : num_cmds;
    for (int i = 0; i < total_cmds; i++) {
        strncpy(cmds_state[i].cmd, cmds[i], sizeof(cmds_state[i].cmd) - 1);
        cmds_state[i].cmd[sizeof(cmds_state[i].cmd) - 1] = '\0';
        cmds_state[i].pid = -1;
        cmds_state[i].pipe_fd = -1;
        cmds_state[i].running = 0;
        cmds_state[i].last_finish_time = 0;

        spawn_command(ctx, &cmds_state[i]);
    }

    int interrupted = 0;
    char read_buf[PIPE_BUF_SIZE];

    while (!interrupted) {
        if (g_multiwatch_interrupted) {
            interrupted = 1;
            break;
        }

        struct pollfd pfds[MULTIWATCH_MAX_CMDS + 1];
        int cmd_map[MULTIWATCH_MAX_CMDS + 1];
        int nfds = 0;

        /* Watch GUI input pipe for Ctrl+C or tab close */
        int in_idx = -1;
        if (in_fd >= 0) {
            in_idx = nfds;
            pfds[nfds].fd = in_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            cmd_map[nfds] = -1;
            nfds++;
        }

        /* Watch active child process output pipes */
        for (int i = 0; i < total_cmds; i++) {
            if (cmds_state[i].running && cmds_state[i].pipe_fd >= 0) {
                pfds[nfds].fd = cmds_state[i].pipe_fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                cmd_map[nfds] = i;
                nfds++;
            }
        }

        int ret = poll(pfds, nfds, MULTIWATCH_POLL_TIMEOUT_MS);
        if (g_multiwatch_interrupted) {
            interrupted = 1;
            break;
        }

        if (ret > 0) {
            /* 1. Check if Ctrl+C or CLOSE arrived from GUI */
            if (in_idx >= 0 && (pfds[in_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                uint32_t msg_type = 0, msg_len = 0;
                char in_buf[MAX_CMD_LEN];
                if (proto_recv(in_fd, &msg_type, in_buf, sizeof(in_buf), &msg_len) == 0) {
                    if (msg_type == GUI_MSG_SIGNAL || msg_type == GUI_MSG_CLOSE) {
                        interrupted = 1;
                        break;
                    }
                }
            }

            /* 2. Process child pipes */
            for (int k = 0; k < nfds; k++) {
                int cmd_idx = cmd_map[k];
                if (cmd_idx < 0) continue;

                MultiwatchCommand *mc = &cmds_state[cmd_idx];
                if (pfds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
                    ssize_t n = read(mc->pipe_fd, read_buf, sizeof(read_buf) - 1);
                    if (n > 0) {
                        read_buf[n] = '\0';
                        time_t now = time(NULL);

                        /* Print exact required format:
                         * "cmd", <timestamp>:
                         * ---------------------------------------------------
                         * <output>
                         * ---------------------------------------------------
                         */
                        char header[MAX_CMD_LEN + 128];
                        snprintf(header, sizeof(header), "\"%s\", %ld:\n---------------------------------------------------\n",
                                 mc->cmd, (long)now);
                        if (out_fd >= 0) {
                            proto_send(out_fd, TAB_MSG_OUTPUT, header, (uint32_t)strlen(header));
                            proto_send(out_fd, TAB_MSG_OUTPUT, read_buf, (uint32_t)n);
                            if (read_buf[n - 1] != '\n') {
                                proto_send(out_fd, TAB_MSG_OUTPUT, "\n", 1);
                            }
                            const char div[] = "---------------------------------------------------\n";
                            proto_send(out_fd, TAB_MSG_OUTPUT, div, (uint32_t)strlen(div));
                        }
                    } else if (n == 0 || (pfds[k].revents & (POLLHUP | POLLERR))) {
                        /* Process closed pipe / terminated */
                        close(mc->pipe_fd);
                        mc->pipe_fd = -1;
                        waitpid(mc->pid, NULL, 0);
                        mc->pid = -1;
                        mc->running = 0;
                        mc->last_finish_time = time(NULL);
                    }
                }
            }
        }

        /* 3. Re-execution check: re-run any finished command after 2 seconds */
        time_t now = time(NULL);
        for (int i = 0; i < total_cmds; i++) {
            if (!cmds_state[i].running && (now - cmds_state[i].last_finish_time >= MULTIWATCH_INTERVAL_SEC)) {
                spawn_command(ctx, &cmds_state[i]);
            }
        }
    }

    /* Teardown: terminate all children cleanly */
    for (int i = 0; i < total_cmds; i++) {
        if (cmds_state[i].running && cmds_state[i].pid > 0) {
            kill(-cmds_state[i].pid, SIGINT);
        }
    }

    usleep(50000); /* 50ms grace period */

    for (int i = 0; i < total_cmds; i++) {
        if (cmds_state[i].running && cmds_state[i].pid > 0) {
            int status;
            pid_t r = waitpid(cmds_state[i].pid, &status, WNOHANG);
            if (r == 0) {
                kill(-cmds_state[i].pid, SIGKILL);
                waitpid(cmds_state[i].pid, NULL, 0);
            }
            cmds_state[i].running = 0;
        }
        if (cmds_state[i].pipe_fd >= 0) {
            close(cmds_state[i].pipe_fd);
            cmds_state[i].pipe_fd = -1;
        }
    }

    /* Print ^C on Ctrl+C termination */
    if (interrupted && out_fd >= 0) {
        const char intr_msg[] = "^C\n";
        proto_send(out_fd, TAB_MSG_OUTPUT, intr_msg, (uint32_t)strlen(intr_msg));
    }

    /* Restore original SIGINT handler */
    sigaction(SIGINT, &old_sa, NULL);

    if (ctx) ctx->last_status = 130;
    return 130;
}
