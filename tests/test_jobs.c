/*
 * test_jobs.c — Unit tests for signals and job control (Stage 8 / Task 8)
 */
#include "exec/jobs.h"
#include "exec/pipeline.h"
#include "exec/shell_ctx.h"
#include "exec/builtins.h"
#include "ipc/protocol.h"
#include "parser/lexer.h"
#include "parser/ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define TEST_PASS() printf("  ✓ %s passed\n", __func__)

static void test_job_table_basics(void) {
    JobTable *jt = jobs_create();
    assert(jt != NULL);
    assert(jt->count == 0);

    pid_t pids1[] = {1234};
    Job *j1 = jobs_add(jt, 1234, pids1, 1, "sleep 100", JOB_STATE_STOPPED, -1);
    assert(j1 != NULL);
    assert(j1->job_id == 1);
    assert(j1->pgid == 1234);
    assert(j1->state == JOB_STATE_STOPPED);
    assert(j1->is_current == 1);
    assert(jt->count == 1);

    pid_t pids2[] = {5678};
    Job *j2 = jobs_add(jt, 5678, pids2, 1, "cat file.txt | grep foo &", JOB_STATE_RUNNING, -1);
    assert(j2 != NULL);
    assert(j2->job_id == 2);
    assert(j2->is_current == 1);
    assert(j1->is_current == 0);
    assert(jt->count == 2);

    /* Test lookups */
    assert(jobs_find_by_id(jt, 1) == j1);
    assert(jobs_find_by_id(jt, 2) == j2);
    assert(jobs_find_by_id(jt, 3) == NULL);
    assert(jobs_find_by_pgid(jt, 1234) == j1);
    assert(jobs_find_by_pgid(jt, 5678) == j2);
    assert(jobs_find_current(jt) == j2);

    /* Test remove */
    jobs_remove(jt, 1);
    assert(jt->count == 1);
    assert(jobs_find_by_id(jt, 1) == NULL);
    Job *rem2 = jobs_find_by_id(jt, 2);
    assert(rem2 != NULL);
    assert(rem2->job_id == 2);
    assert(rem2->is_current == 1);

    jobs_destroy(jt);
    TEST_PASS();
}

static void test_jobs_list_and_bg_format(void) {
    JobTable *jt = jobs_create();
    assert(jt != NULL);

    int pipe_fd[2];
    assert(pipe(pipe_fd) == 0);

    /* Add stopped job */
    pid_t pids[] = {4321};
    jobs_add(jt, 4321, pids, 1, "sleep 100", JOB_STATE_STOPPED, -1);

    /* Test jobs_list output format */
    jobs_list(jt, pipe_fd[1]);

    uint32_t msg_type = 0, msg_len = 0;
    char buf[512];
    assert(proto_recv(pipe_fd[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';

    /* Exact bash format: "[1]+  Stopped                 sleep 100\n" */
    assert(strcmp(buf, "[1]+  Stopped                 sleep 100\n") == 0);

    /* Spawn a real process to test jobs_bg with SIGCONT */
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        raise(SIGSTOP);
        /* Sleep until killed */
        sleep(30);
        _exit(0);
    }
    setpgid(child, child);
    int status;
    waitpid(child, &status, WUNTRACED);
    assert(WIFSTOPPED(status));

    JobTable *jt_real = jobs_create();
    pid_t cpids[] = {child};
    Job *jr = jobs_add(jt_real, child, cpids, 1, "sleep 30", JOB_STATE_STOPPED, -1);
    assert(jr != NULL);

    /* Call jobs_bg */
    int bg_res = jobs_bg(jt_real, 1, pipe_fd[1]);
    assert(bg_res == 0);
    assert(jr->state == JOB_STATE_RUNNING);

    /* Check bg message output */
    assert(proto_recv(pipe_fd[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    /* Exact transcript format: "[1]+ sleep 30 &\n" */
    assert(strcmp(buf, "[1]+ sleep 30 &\n") == 0);

    /* Clean up child process */
    kill(-child, SIGKILL);
    waitpid(child, NULL, 0);

    jobs_destroy(jt);
    jobs_destroy(jt_real);
    close(pipe_fd[0]);
    close(pipe_fd[1]);

    TEST_PASS();
}

static void test_background_command_execution(void) {
    ShellContext *ctx = shell_ctx_create(0);
    assert(ctx != NULL);

    int pipe_fd[2];
    assert(pipe(pipe_fd) == 0);

    /* Parse "sleep 1 &" */
    char *tokens[MAX_ARGS];
    char cmd_str[] = "sleep 1 &";
    char copy[128];
    strcpy(copy, cmd_str);
    int num_tokens = lexer_tokenize(copy, tokens, MAX_ARGS);

    Pipeline pl;
    assert(ast_parse(tokens, num_tokens, &pl) == 0);
    assert(pl.is_background == 1);
    strncpy(pl.cmd_str, "sleep 1 &", sizeof(pl.cmd_str) - 1);

    /* Execute in background */
    int res = pipeline_exec_pipeline(ctx, &pl, pipe_fd[1]);
    assert(res == 0);
    assert(ctx->jobs->count == 1);

    /* Read bg start message "[1] <pgid>\n" */
    uint32_t msg_type = 0, msg_len = 0;
    char buf[512];
    assert(proto_recv(pipe_fd[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    assert(buf[0] == '[' && buf[1] == '1' && buf[2] == ']');

    /* While sleep 1 is running, jobs_check_status should not reap it */
    jobs_check_status(ctx->jobs, pipe_fd[1]);
    assert(ctx->jobs->count == 1);

    /* Wait 1.2s for sleep 1 to terminate */
    usleep(1200000);

    /* jobs_check_status should now reap and report "[1]+  Done                    sleep 1\n" */
    jobs_check_status(ctx->jobs, pipe_fd[1]);
    assert(ctx->jobs->count == 0);

    assert(proto_recv(pipe_fd[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    assert(strstr(buf, "Done") != NULL);
    assert(strstr(buf, "sleep 1") != NULL);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num_tokens);
    shell_ctx_destroy(ctx);
    close(pipe_fd[0]);
    close(pipe_fd[1]);

    TEST_PASS();
}

static void test_foreground_ctrl_c(void) {
    ShellContext *ctx = shell_ctx_create(0);
    assert(ctx != NULL);

    int out_pipe[2];
    int in_pipe[2];
    assert(pipe(out_pipe) == 0);
    assert(pipe(in_pipe) == 0);

    ctx->in_fd = in_pipe[0];

    /* Fork a child process simulating foreground job: sleep 10 */
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        execlp("sleep", "sleep", "10", (char *)NULL);
        _exit(127);
    }
    setpgid(child, child);

    /* Feed Ctrl+C (SIGINT) to in_pipe */
    int sig = SIGINT;
    proto_send(in_pipe[1], GUI_MSG_SIGNAL, (const char *)&sig, sizeof(int));

    /* Wait foreground */
    pid_t pids[] = {child};
    int res = pipeline_wait_foreground(ctx, child, pids, 1, "sleep 10", -1, out_pipe[1], in_pipe[0]);
    assert(res == 130);

    /* Verify ^C was output */
    uint32_t msg_type = 0, msg_len = 0;
    char buf[512];
    assert(proto_recv(out_pipe[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    assert(strcmp(buf, "^C\n") == 0);

    shell_ctx_destroy(ctx);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(in_pipe[0]);
    close(in_pipe[1]);

    TEST_PASS();
}

static void test_foreground_ctrl_z_and_fg(void) {
    ShellContext *ctx = shell_ctx_create(0);
    assert(ctx != NULL);

    int out_pipe[2];
    int in_pipe[2];
    assert(pipe(out_pipe) == 0);
    assert(pipe(in_pipe) == 0);

    ctx->in_fd = in_pipe[0];

    /* Fork a child process: sleep 10 */
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        execlp("sleep", "sleep", "10", (char *)NULL);
        _exit(127);
    }
    setpgid(child, child);

    /* Feed Ctrl+Z (SIGTSTP) to in_pipe */
    int sig = SIGTSTP;
    proto_send(in_pipe[1], GUI_MSG_SIGNAL, (const char *)&sig, sizeof(int));

    /* Wait foreground */
    pid_t pids[] = {child};
    int res = pipeline_wait_foreground(ctx, child, pids, 1, "sleep 10", -1, out_pipe[1], in_pipe[0]);
    assert(res == 148);

    /* Verify ^Z and stopped status output */
    uint32_t msg_type = 0, msg_len = 0;
    char buf[512];
    assert(proto_recv(out_pipe[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    assert(strcmp(buf, "^Z\n[1]+  Stopped                 sleep 10\n") == 0);

    /* Verify job table has the stopped job */
    assert(ctx->jobs->count == 1);
    Job *j = jobs_find_by_id(ctx->jobs, 1);
    assert(j != NULL);
    assert(j->state == JOB_STATE_STOPPED);

    /* Now bring it to foreground with fg %1, and send Ctrl+C to terminate it */
    sig = SIGINT;
    proto_send(in_pipe[1], GUI_MSG_SIGNAL, (const char *)&sig, sizeof(int));

    char *fg_argv[] = {"fg", "%1", NULL};
    res = builtin_fg(ctx, 2, fg_argv, out_pipe[1]);
    assert(res == 130);

    /* Verify fg outputs "sleep 10\n" and then "^C\n" */
    assert(proto_recv(out_pipe[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    assert(strcmp(buf, "sleep 10\n") == 0);

    assert(proto_recv(out_pipe[0], &msg_type, buf, sizeof(buf), &msg_len) == 0);
    buf[msg_len] = '\0';
    assert(strcmp(buf, "^C\n") == 0);

    /* Job should be removed from job table */
    assert(ctx->jobs->count == 0);

    shell_ctx_destroy(ctx);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(in_pipe[0]);
    close(in_pipe[1]);

    TEST_PASS();
}

int main(void) {
    printf("\n=== Running Signals & Job Control Unit Tests (Stage 8) ===\n");
    test_job_table_basics();
    test_jobs_list_and_bg_format();
    test_background_command_execution();
    test_foreground_ctrl_c();
    test_foreground_ctrl_z_and_fg();
    printf("✓ All Stage 8 Signals & Job Control unit tests passed!\n\n");
    return 0;
}
