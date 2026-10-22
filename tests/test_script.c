/*
 * test_script.c — Unit & integration tests for bash script interpreter (Stage 11 / Task 11)
 */
#include "script/script_validator.h"
#include "script/test_eval.h"
#include "script/interpreter.h"
#include "exec/shell_ctx.h"
#include "ipc/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_PASS() printf("  ✓ %s passed\n", __func__)

static void collect_output(int fd, char *out, size_t max_size) {
    if (!out || max_size == 0) return;
    out[0] = '\0';
    size_t total = 0;
    uint32_t type = 0, len = 0;
    char chunk[1024];
    while (proto_recv(fd, &type, chunk, sizeof(chunk) - 1, &len) == 0) {
        chunk[len] = '\0';
        size_t to_copy = len;
        if (total + to_copy >= max_size) to_copy = max_size - total - 1;
        memcpy(out + total, chunk, to_copy);
        total += to_copy;
        out[total] = '\0';
    }
}

static void test_validator_balanced_and_syntax_errors(void) {
    ScriptValidationResult res;

    /* 1. Valid script */
    const char *valid_script =
        "#!/bin/bash\n"
        "if [ -f \"$1\" ]; then\n"
        "    echo \"File exists\"\n"
        "else\n"
        "    echo \"Missing\"\n"
        "fi\n";
    assert(script_validator_check_text(valid_script, &res) == 0);
    assert(res.is_valid == 1);

    /* 2. Valid nested if */
    const char *nested_script =
        "if [ $a -gt 1 ]; then\n"
        "    if [ $b -gt 2 ]; then\n"
        "        echo \"both\"\n"
        "    fi\n"
        "elif [ $a -eq 0 ]; then\n"
        "    echo \"zero\"\n"
        "else\n"
        "    echo \"other\"\n"
        "fi\n";
    assert(script_validator_check_text(nested_script, &res) == 0);
    assert(res.is_valid == 1);

    /* 3. Missing fi */
    const char *missing_fi =
        "#!/bin/bash\n"
        "if [ -f \"$1\" ]; then\n"
        "    echo \"File exists\"\n"
        "else\n"
        "    echo \"Missing\"\n";
    assert(script_validator_check_text(missing_fi, &res) < 0);
    assert(res.is_valid == 0);
    assert(strstr(res.error_msg, "missing 'fi'") != NULL);

    /* 4. Unexpected fi */
    const char *unexpected_fi =
        "echo \"hello\"\n"
        "fi\n";
    assert(script_validator_check_text(unexpected_fi, &res) < 0);
    assert(res.is_valid == 0);
    assert(strstr(res.error_msg, "unexpected token 'fi'") != NULL);

    /* 5. Missing then */
    const char *missing_then =
        "if [ $x -eq 1 ]\n"
        "echo \"no then\"\n"
        "fi\n";
    assert(script_validator_check_text(missing_then, &res) < 0);
    assert(res.is_valid == 0);

    /* 6. Unclosed quote */
    const char *unclosed_quote =
        "echo \"hello world\n"
        "echo next\n";
    assert(script_validator_check_text(unclosed_quote, &res) < 0);
    assert(res.is_valid == 0);

    TEST_PASS();
}

static void test_condition_evaluator(void) {
    ShellContext *ctx = shell_ctx_create(0);
    assert(ctx != NULL);

    /* String tests */
    assert(test_eval_expression(ctx, "[ \"abc\" = \"abc\" ]") == 0);
    assert(test_eval_expression(ctx, "[ \"abc\" != \"def\" ]") == 0);
    assert(test_eval_expression(ctx, "[ \"abc\" = \"xyz\" ]") == 1);
    assert(test_eval_expression(ctx, "[ -z \"\" ]") == 0);
    assert(test_eval_expression(ctx, "[ -z \"nonempty\" ]") == 1);
    assert(test_eval_expression(ctx, "[ -n \"nonempty\" ]") == 0);
    assert(test_eval_expression(ctx, "[ -n \"\" ]") == 1);

    /* Integer tests */
    assert(test_eval_expression(ctx, "[ 5 -eq 5 ]") == 0);
    assert(test_eval_expression(ctx, "[ 5 -ne 6 ]") == 0);
    assert(test_eval_expression(ctx, "[ 10 -gt 5 ]") == 0);
    assert(test_eval_expression(ctx, "[ 5 -gt 10 ]") == 1);
    assert(test_eval_expression(ctx, "[ 5 -le 5 ]") == 0);
    assert(test_eval_expression(ctx, "[ 3 -lt 5 ]") == 0);

    /* Inversion */
    assert(test_eval_expression(ctx, "[ ! 5 -eq 6 ]") == 0);

    /* Logical AND and OR */
    assert(test_eval_expression(ctx, "[ 5 -eq 5 -a \"a\" = \"a\" ]") == 0);
    assert(test_eval_expression(ctx, "[ 5 -eq 6 -o \"a\" = \"a\" ]") == 0);
    assert(test_eval_expression(ctx, "[ 5 -eq 6 -a \"a\" = \"a\" ]") == 1);

    /* File tests */
    const char *test_file = "/tmp/test_eval_dummy.txt";
    FILE *f = fopen(test_file, "w");
    if (f) { fprintf(f, "test"); fclose(f); }

    char expr[256];
    snprintf(expr, sizeof(expr), "[ -f \"%s\" ]", test_file);
    assert(test_eval_expression(ctx, expr) == 0);

    snprintf(expr, sizeof(expr), "[ -d \"%s\" ]", test_file);
    assert(test_eval_expression(ctx, expr) == 1);

    assert(test_eval_expression(ctx, "[ -d \"/tmp\" ]") == 0);
    assert(test_eval_expression(ctx, "[ -f \"/tmp/nonexistent_xyz_123.txt\" ]") == 1);

    unlink(test_file);
    shell_ctx_destroy(ctx);
    TEST_PASS();
}

static void test_section13_check_script(void) {
    const char *sandbox = "/tmp/test_terminator_script_sandbox";
    mkdir(sandbox, 0755);

    /* Write myprog.c */
    char c_file[512];
    snprintf(c_file, sizeof(c_file), "%s/myprog.c", sandbox);
    FILE *fc = fopen(c_file, "w");
    assert(fc != NULL);
    fprintf(fc, "#include <stdio.h>\nint main(void) { printf(\"compiled\\n\"); return 0; }\n");
    fclose(fc);

    /* Write check.sh */
    char sh_file[512];
    snprintf(sh_file, sizeof(sh_file), "%s/check.sh", sandbox);
    FILE *fs = fopen(sh_file, "w");
    assert(fs != NULL);
    fprintf(fs,
            "#!/bin/bash\n"
            "if [ -f \"$1\" ]; then\n"
            "    echo \"File exists, compiling...\"\n"
            "    gcc \"$1\" -o myprog\n"
            "else\n"
            "    echo \"Source file missing!\"\n"
            "fi\n");
    fclose(fs);

    ShellContext *ctx = shell_ctx_create(0);
    assert(ctx != NULL);
    shell_ctx_set_cwd(ctx, sandbox);

    /* 1. Test "./check.sh myprog.c" */
    int pipefd[2];
    assert(pipe(pipefd) == 0);

    char *argv_exists[] = {"./check.sh", "myprog.c", NULL};
    int status = interpreter_run_file(ctx, "./check.sh", 2, argv_exists, 0, pipefd[1]);
    close(pipefd[1]);
    assert(status == 0);

    char out_buf[1024] = {0};
    collect_output(pipefd[0], out_buf, sizeof(out_buf));
    close(pipefd[0]);
    assert(strstr(out_buf, "File exists, compiling...") != NULL);

    /* Verify gcc produced myprog */
    char prog_path[512];
    snprintf(prog_path, sizeof(prog_path), "%s/myprog", sandbox);
    assert(access(prog_path, X_OK) == 0);

    /* 2. Test "bash check.sh nothere.c" */
    assert(pipe(pipefd) == 0);
    char *argv_missing[] = {"bash", "check.sh", "nothere.c", NULL};
    status = interpreter_run_file(ctx, "check.sh", 2, &argv_missing[1], 0, pipefd[1]);
    close(pipefd[1]);
    assert(status == 0);

    memset(out_buf, 0, sizeof(out_buf));
    collect_output(pipefd[0], out_buf, sizeof(out_buf));
    close(pipefd[0]);
    assert(strstr(out_buf, "Source file missing!") != NULL);

    /* 3. Test script with missing fi -> pre-execution syntax error abort */
    char bad_sh[512];
    snprintf(bad_sh, sizeof(bad_sh), "%s/bad.sh", sandbox);
    FILE *fb = fopen(bad_sh, "w");
    assert(fb != NULL);
    fprintf(fb,
            "#!/bin/bash\n"
            "if [ -f \"$1\" ]; then\n"
            "    echo \"Should not run!\"\n");
    fclose(fb);

    assert(pipe(pipefd) == 0);
    char *argv_bad[] = {"./bad.sh", "file.txt", NULL};
    status = interpreter_run_file(ctx, "./bad.sh", 2, argv_bad, 0, pipefd[1]);
    close(pipefd[1]);
    assert(status == 2); /* Syntax error code */

    memset(out_buf, 0, sizeof(out_buf));
    collect_output(pipefd[0], out_buf, sizeof(out_buf));
    close(pipefd[0]);
    assert(strstr(out_buf, "syntax error") != NULL);
    assert(strstr(out_buf, "Should not run!") == NULL);

    /* Cleanup */
    unlink(c_file);
    unlink(sh_file);
    unlink(bad_sh);
    unlink(prog_path);
    rmdir(sandbox);
    shell_ctx_destroy(ctx);

    TEST_PASS();
}

static void test_variables_and_exit(void) {
    const char *sandbox = "/tmp/test_terminator_script_vars";
    mkdir(sandbox, 0755);

    char sh_file[512];
    snprintf(sh_file, sizeof(sh_file), "%s/vars.sh", sandbox);
    FILE *fs = fopen(sh_file, "w");
    assert(fs != NULL);
    fprintf(fs,
            "FOO=hello\n"
            "BAR=world\n"
            "echo \"$FOO $BAR from script\"\n"
            "exit 42\n"
            "echo \"unreachable\"\n");
    fclose(fs);

    ShellContext *ctx = shell_ctx_create(0);
    assert(ctx != NULL);
    shell_ctx_set_cwd(ctx, sandbox);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    char *argv[] = {"./vars.sh", NULL};
    int status = interpreter_run_file(ctx, "./vars.sh", 1, argv, 0, pipefd[1]);
    close(pipefd[1]);

    assert(status == 42);

    char out_buf[1024] = {0};
    collect_output(pipefd[0], out_buf, sizeof(out_buf));
    close(pipefd[0]);
    assert(strstr(out_buf, "hello world from script") != NULL);
    assert(strstr(out_buf, "unreachable") == NULL);

    unlink(sh_file);
    rmdir(sandbox);
    shell_ctx_destroy(ctx);

    TEST_PASS();
}

int main(void) {
    printf("\n=== Running Bash Script Interpreter Unit Tests (Stage 11) ===\n");
    test_validator_balanced_and_syntax_errors();
    test_condition_evaluator();
    test_section13_check_script();
    test_variables_and_exit();
    printf("✓ All Stage 11 Bash Script Interpreter unit tests passed!\n\n");
    return 0;
}
