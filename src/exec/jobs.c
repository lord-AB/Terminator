/*
 * jobs.c — Job control table, process group tracking, and jobs/bg/fg built-ins
 */
#include "exec/jobs.h"
#include "exec/shell_ctx.h"
#include "exec/pipeline.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

JobTable *jobs_create(void) {
    JobTable *jt = calloc(1, sizeof(JobTable));
    if (!jt) return NULL;
    jt->next_job_id = 1;
    return jt;
}

void jobs_destroy(JobTable *jt) {
    if (!jt) return;

    for (int i = 0; i < jt->count; i++) {
        Job *j = &jt->jobs[i];
        if (j->pgid > 0) {
            kill(-j->pgid, SIGHUP);
            kill(-j->pgid, SIGCONT);
            usleep(20000);
            kill(-j->pgid, SIGKILL);
            for (int k = 0; k < j->num_pids; k++) {
                waitpid(j->pids[k], NULL, WNOHANG);
            }
        }
        if (j->capture_fd >= 0) {
            close(j->capture_fd);
            j->capture_fd = -1;
        }
    }
    free(jt);
}

Job *jobs_add(JobTable *jt, pid_t pgid, pid_t pids[], int num_pids,
              const char *cmd, JobState state, int capture_fd) {
    if (!jt || jt->count >= MAX_JOBS) return NULL;

    /* Clear current marker on previous jobs */
    for (int i = 0; i < jt->count; i++) {
        jt->jobs[i].is_current = 0;
    }

    Job *j = &jt->jobs[jt->count];
    memset(j, 0, sizeof(Job));

    j->job_id = jt->next_job_id++;
    j->pgid = pgid;
    j->num_pids = num_pids > MAX_COMMANDS_IN_PIPELINE ? MAX_COMMANDS_IN_PIPELINE : num_pids;
    for (int k = 0; k < j->num_pids; k++) {
        j->pids[k] = pids[k];
    }
    if (cmd) {
        strncpy(j->cmd, cmd, sizeof(j->cmd) - 1);
        j->cmd[sizeof(j->cmd) - 1] = '\0';
    }
    j->state = state;
    j->capture_fd = capture_fd;
    j->is_current = 1;

    jt->count++;
    return j;
}

Job *jobs_find_by_id(JobTable *jt, int job_id) {
    if (!jt || job_id <= 0) return NULL;
    for (int i = 0; i < jt->count; i++) {
        if (jt->jobs[i].job_id == job_id) {
            return &jt->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_find_by_pgid(JobTable *jt, pid_t pgid) {
    if (!jt || pgid <= 0) return NULL;
    for (int i = 0; i < jt->count; i++) {
        if (jt->jobs[i].pgid == pgid) {
            return &jt->jobs[i];
        }
    }
    return NULL;
}

Job *jobs_find_current(JobTable *jt) {
    if (!jt || jt->count == 0) return NULL;
    for (int i = jt->count - 1; i >= 0; i--) {
        if (jt->jobs[i].is_current) {
            return &jt->jobs[i];
        }
    }
    return &jt->jobs[jt->count - 1];
}

void jobs_remove(JobTable *jt, int job_id) {
    if (!jt || job_id <= 0) return;

    for (int i = 0; i < jt->count; i++) {
        if (jt->jobs[i].job_id == job_id) {
            if (jt->jobs[i].capture_fd >= 0) {
                close(jt->jobs[i].capture_fd);
                jt->jobs[i].capture_fd = -1;
            }
            /* Shift left */
            for (int k = i; k < jt->count - 1; k++) {
                jt->jobs[k] = jt->jobs[k + 1];
            }
            jt->count--;
            if (jt->count == 0) {
                jt->next_job_id = 1;
            } else {
                jt->jobs[jt->count - 1].is_current = 1;
            }
            return;
        }
    }
}

static void clean_cmd_name(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    size_t len = strlen(dst);
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\t' || dst[len - 1] == '&')) {
        dst[--len] = '\0';
    }
}

void jobs_check_status(JobTable *jt, int out_fd) {
    if (!jt) return;

    for (int i = 0; i < jt->count; i++) {
        Job *j = &jt->jobs[i];
        int status = 0;
        pid_t r = waitpid(-j->pgid, &status, WNOHANG | WUNTRACED);
        if (r > 0) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                char clean_cmd[MAX_CMD_LEN];
                clean_cmd_name(j->cmd, clean_cmd, sizeof(clean_cmd));
                char marker = j->is_current ? '+' : ' ';
                char msg[MAX_CMD_LEN + 64];
                snprintf(msg, sizeof(msg), "[%d]%c  Done                    %s\n", j->job_id, marker, clean_cmd);
                proto_send_text(out_fd, msg);
                jobs_remove(jt, j->job_id);
                i--;
            } else if (WIFSTOPPED(status)) {
                j->state = JOB_STATE_STOPPED;
            }
        }
    }
}

int jobs_list(JobTable *jt, int out_fd) {
    if (!jt) return 0;

    jobs_check_status(jt, out_fd);

    for (int i = 0; i < jt->count; i++) {
        Job *j = &jt->jobs[i];
        char line[MAX_CMD_LEN + 64];
        char clean_cmd[MAX_CMD_LEN];
        clean_cmd_name(j->cmd, clean_cmd, sizeof(clean_cmd));
        char marker = j->is_current ? '+' : ' ';
        if (j->state == JOB_STATE_STOPPED) {
            snprintf(line, sizeof(line), "[%d]%c  Stopped                 %s\n", j->job_id, marker, clean_cmd);
        } else {
            snprintf(line, sizeof(line), "[%d]%c  Running                 %s &\n", j->job_id, marker, clean_cmd);
        }
        proto_send_text(out_fd, line);
    }
    return 0;
}

int jobs_bg(JobTable *jt, int job_id, int out_fd) {
    if (!jt) return 1;

    Job *j = (job_id > 0) ? jobs_find_by_id(jt, job_id) : jobs_find_current(jt);
    if (!j) {
        char err[] = "terminator: bg: no such job\n";
        proto_send_text(out_fd, err);
        return 1;
    }

    if (j->state == JOB_STATE_RUNNING) {
        char err[64];
        snprintf(err, sizeof(err), "terminator: bg: job %d already in background\n", j->job_id);
        proto_send_text(out_fd, err);
        return 0;
    }

    /* Send SIGCONT to the entire process group */
    if (kill(-j->pgid, SIGCONT) < 0) {
        char err[128];
        snprintf(err, sizeof(err), "terminator: bg: %s\n", strerror(errno));
        proto_send_text(out_fd, err);
        return 1;
    }

    j->state = JOB_STATE_RUNNING;

    /* Output: "[1]+ sleep 100 &" */
    char clean_cmd[MAX_CMD_LEN];
    clean_cmd_name(j->cmd, clean_cmd, sizeof(clean_cmd));
    char msg[MAX_CMD_LEN + 64];
    snprintf(msg, sizeof(msg), "[%d]+ %s &\n", j->job_id, clean_cmd);
    proto_send_text(out_fd, msg);

    return 0;
}

int jobs_fg(ShellContext *ctx, JobTable *jt, int job_id, int out_fd, int in_fd) {
    if (!jt || !ctx) return 1;

    Job *j = (job_id > 0) ? jobs_find_by_id(jt, job_id) : jobs_find_current(jt);
    if (!j) {
        char err[] = "terminator: fg: no such job\n";
        proto_send_text(out_fd, err);
        return 1;
    }

    /* Print command name: "sleep 100" */
    char clean_cmd[MAX_CMD_LEN];
    clean_cmd_name(j->cmd, clean_cmd, sizeof(clean_cmd));
    char msg[MAX_CMD_LEN + 64];
    snprintf(msg, sizeof(msg), "%s\n", clean_cmd);
    proto_send_text(out_fd, msg);

    /* Resume job */
    kill(-j->pgid, SIGCONT);
    j->state = JOB_STATE_RUNNING;

    /* Copy job details before wait */
    pid_t pgid = j->pgid;
    pid_t pids[MAX_COMMANDS_IN_PIPELINE];
    int num_pids = j->num_pids;
    for (int k = 0; k < num_pids; k++) pids[k] = j->pids[k];
    char cmd_copy[MAX_CMD_LEN];
    strncpy(cmd_copy, j->cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    int capture_fd = j->capture_fd;
    int jid = j->job_id;

    /* Wait in foreground */
    int res = pipeline_wait_foreground(ctx, pgid, pids, num_pids, cmd_copy, capture_fd, out_fd, in_fd);

    /* If job was stopped again (WIFSTOPPED), state was updated in wait loop */
    Job *check = jobs_find_by_id(jt, jid);
    if (check && check->state != JOB_STATE_STOPPED) {
        jobs_remove(jt, jid);
    }

    return res;
}
