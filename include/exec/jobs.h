/*
 * jobs.h — Job control table, process group management, fg/bg/jobs built-ins
 */
#ifndef EXEC_JOBS_H
#define EXEC_JOBS_H

#include "common.h"
#include "parser/ast.h"
#include <sys/types.h>
#include <unistd.h>

#define MAX_JOBS 32

struct ShellContext; /* Forward declaration */

typedef enum {
    JOB_STATE_RUNNING,
    JOB_STATE_STOPPED,
    JOB_STATE_DONE
} JobState;

typedef struct {
    int      job_id;                    /* 1-based job index (1, 2, ...) */
    pid_t    pgid;                      /* Process group ID */
    pid_t    pids[MAX_COMMANDS_IN_PIPELINE];
    int      num_pids;
    char     cmd[MAX_CMD_LEN];          /* Command line string */
    JobState state;
    int      capture_fd;                /* Pipe capture read end */
    int      is_current;                /* 1 if current job (+) */
} Job;

typedef struct JobTable {
    Job jobs[MAX_JOBS];
    int count;
    int next_job_id;
} JobTable;

/* Allocate and initialize a job table */
JobTable *jobs_create(void);

/* Free and terminate all jobs in the table */
void jobs_destroy(JobTable *jt);

/* Add a new job to the table */
Job *jobs_add(JobTable *jt, pid_t pgid, pid_t pids[], int num_pids,
              const char *cmd, JobState state, int capture_fd);

/* Find job by 1-based job ID */
Job *jobs_find_by_id(JobTable *jt, int job_id);

/* Find job by process group ID */
Job *jobs_find_by_pgid(JobTable *jt, pid_t pgid);

/* Find current job (the '+' job) */
Job *jobs_find_current(JobTable *jt);

/* Remove a job by ID */
void jobs_remove(JobTable *jt, int job_id);

/* Check and reap completed background jobs non-blockingly (WNOHANG) */
void jobs_check_status(JobTable *jt, int out_fd);

/* Built-in 'jobs': display all active jobs */
int jobs_list(JobTable *jt, int out_fd);

/* Built-in 'bg': send SIGCONT to job and mark running in background */
int jobs_bg(JobTable *jt, int job_id, int out_fd);

/* Built-in 'fg': bring job to foreground and wait */
int jobs_fg(struct ShellContext *ctx, JobTable *jt, int job_id, int out_fd, int in_fd);

#endif /* EXEC_JOBS_H */
