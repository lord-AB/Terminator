/*
 * test_multiwatch.c — Unit tests for multiWatch parsing, parallel execution, and Ctrl+C termination
 */
#include "features/multiwatch.h"
#include "exec/shell_ctx.h"
#include "ipc/protocol.h"
#include "parser/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define TEST_PASS() printf("  ✓ %s passed\n", __func__)

static void test_parse_arguments(void) {
    char *cmds[MULTIWATCH_MAX_CMDS];

    /* Test 1: Standard quoted bracket format */
    char *argv1[] = {"multiWatch", "[date +%T,", "ls | wc -l]"};
    int n = multiwatch_parse_args(3, argv1, cmds, MULTIWATCH_MAX_CMDS);
    assert(n == 2);
    assert(strcmp(cmds[0], "date +%T") == 0);
    assert(strcmp(cmds[1], "ls | wc -l") == 0);
    multiwatch_free_cmds(cmds, n);

    /* Test 2: Spaced bracket format */
    char *argv2[] = {"multiWatch", "[", "date +%T", ",", "ls | wc -l", "]"};
    n = multiwatch_parse_args(6, argv2, cmds, MULTIWATCH_MAX_CMDS);
    assert(n == 2);
    assert(strcmp(cmds[0], "date +%T") == 0);
    assert(strcmp(cmds[1], "ls | wc -l") == 0);
    multiwatch_free_cmds(cmds, n);

    /* Test 3: Commands containing internal commas */
    char *argv3[] = {"multiWatch", "[echo hello, world,", "date]"};
    n = multiwatch_parse_args(3, argv3, cmds, MULTIWATCH_MAX_CMDS);
    assert(n == 2);
    assert(strcmp(cmds[0], "echo hello, world") == 0);
    assert(strcmp(cmds[1], "date") == 0);
    multiwatch_free_cmds(cmds, n);

    /* Test 4: Single command bracket format */
    char *argv4[] = {"multiWatch", "[date +%T]"};
    n = multiwatch_parse_args(2, argv4, cmds, MULTIWATCH_MAX_CMDS);
    assert(n == 1);
    assert(strcmp(cmds[0], "date +%T") == 0);
    multiwatch_free_cmds(cmds, n);

    /* Test 5: Lexer-tokenized string end-to-end */
    char *tokens[32];
    int n_tok = lexer_tokenize("multiWatch [\"date +%T\", \"ls | wc -l\"]", tokens, 32);
    assert(n_tok == 3);
    n = multiwatch_parse_args(n_tok, tokens, cmds, MULTIWATCH_MAX_CMDS);
    assert(n == 2);
    assert(strcmp(cmds[0], "date +%T") == 0);
    assert(strcmp(cmds[1], "ls | wc -l") == 0);
    multiwatch_free_cmds(cmds, n);
    lexer_free_tokens(tokens, n_tok);

    /* Test 6: Empty bracket */
    char *argv6[] = {"multiWatch", "[]"};
    n = multiwatch_parse_args(2, argv6, cmds, MULTIWATCH_MAX_CMDS);
    assert(n == 0);

    TEST_PASS();
}

static void test_output_formatting_and_execution(void) {
    int out_pipe[2];
    int in_pipe[2];
    assert(pipe(out_pipe) == 0);
    assert(pipe(in_pipe) == 0);

    ShellContext *ctx = shell_ctx_create(0);
    char *cmds[] = {"echo HELLO_MW", "echo WORLD_MW"};

    /* Fork child to run multiwatch */
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(out_pipe[0]);
        close(in_pipe[1]);
        multiwatch_run(ctx, 2, cmds, out_pipe[1], in_pipe[0]);
        _exit(0);
    }

    close(out_pipe[1]);
    close(in_pipe[0]);

    /* Read output from multiwatch */
    char full_output[4096];
    size_t total_read = 0;
    full_output[0] = '\0';

    /* Let it run for 400ms to produce first iteration of outputs */
    usleep(400000);

    /* Send SIGINT to stop multiwatch cleanly */
    int sig = SIGINT;
    proto_send(in_pipe[1], GUI_MSG_SIGNAL, &sig, sizeof(sig));

    /* Read all remaining packets */
    uint32_t type, len;
    char buf[1024];
    while (proto_recv(out_pipe[0], &type, buf, sizeof(buf) - 1, &len) == 0) {
        buf[len] = '\0';
        if (total_read + len < sizeof(full_output) - 1) {
            strcat(full_output, buf);
            total_read += len;
        }
    }

    waitpid(pid, NULL, 0);
    close(out_pipe[0]);
    close(in_pipe[1]);
    shell_ctx_destroy(ctx);

    /* Verify output format:
     * 1. Contains "echo HELLO_MW"
     * 2. Contains 51 hyphens divider: ---------------------------------------------------
     * 3. Contains HELLO_MW
     * 4. Contains ^C
     */
    assert(strstr(full_output, "\"echo HELLO_MW\",") != NULL);
    assert(strstr(full_output, "---------------------------------------------------") != NULL);
    assert(strstr(full_output, "HELLO_MW") != NULL);
    assert(strstr(full_output, "WORLD_MW") != NULL);
    assert(strstr(full_output, "^C") != NULL);

    TEST_PASS();
}

static void test_clean_child_process_termination(void) {
    int out_pipe[2];
    int in_pipe[2];
    assert(pipe(out_pipe) == 0);
    assert(pipe(in_pipe) == 0);

    ShellContext *ctx = shell_ctx_create(0);
    /* Commands that would run forever if not killed */
    char *cmds[] = {"sleep 60", "sleep 60"};

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(out_pipe[0]);
        close(in_pipe[1]);
        int res = multiwatch_run(ctx, 2, cmds, out_pipe[1], in_pipe[0]);
        _exit(res);
    }

    close(out_pipe[1]);
    close(in_pipe[0]);

    /* Give children time to spawn */
    usleep(150000);

    /* Interrupt via pipe signal */
    int sig = SIGINT;
    proto_send(in_pipe[1], GUI_MSG_SIGNAL, &sig, sizeof(sig));

    int status = 0;
    waitpid(pid, &status, 0);

    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 130);

    close(out_pipe[0]);
    close(in_pipe[1]);
    shell_ctx_destroy(ctx);

    TEST_PASS();
}

int main(void) {
    printf("=== Running Task 6 multiWatch Tests ===\n");
    test_parse_arguments();
    test_output_formatting_and_execution();
    test_clean_child_process_termination();
    printf("✓ All multiWatch tests passed successfully!\n");
    return 0;
}
