/*
 * test_pipeline.c — Unit tests for multi-stage pipelines and pipe orchestration
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include "parser/lexer.h"
#include "parser/ast.h"
#include "exec/pipeline.h"
#include "exec/shell_ctx.h"
#include "ipc/protocol.h"

int main(void) {
    printf("\n=== Running Pipeline & Globbing Unit Tests ===\n");

    ShellContext *ctx = shell_ctx_create(0);
    strncpy(ctx->cwd, "/tmp", sizeof(ctx->cwd));

    char *tokens[MAX_ARGS];
    Pipeline pl;
    int num;
    int pfd[2];
    uint32_t type, len;
    char buf[1024];

    /* ── 1. Create test files in /tmp ────────────────────────────────── */
    /* Files for globbing: /tmp/test1.txt, /tmp/test2.txt, /tmp/test3.txt */
    unlink("/tmp/test1.txt");
    unlink("/tmp/test2.txt");
    unlink("/tmp/test3.txt");

    FILE *f1 = fopen("/tmp/test1.txt", "w");
    assert(f1 != NULL);
    fprintf(f1, "file1 content\n");
    fclose(f1);

    FILE *f2 = fopen("/tmp/test2.txt", "w");
    assert(f2 != NULL);
    fprintf(f2, "file2 content\n");
    fclose(f2);

    FILE *f3 = fopen("/tmp/test3.txt", "w");
    assert(f3 != NULL);
    fprintf(f3, "file3 content\n");
    fclose(f3);

    /* ── 2. Test 2-stage pipeline with wildcards: ls test*.txt | wc -l ── */
    printf("Testing 2-stage pipeline with globbing (ls test*.txt | wc -l)... ");
    num = lexer_tokenize("ls test*.txt | wc -l", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 2);

    assert(pipe(pfd) == 0);
    int status = pipeline_exec_pipeline(ctx, &pl, pfd[1]);
    close(pfd[1]);
    assert(status == 0);

    assert(proto_recv(pfd[0], &type, buf, sizeof(buf) - 1, &len) == 0);
    close(pfd[0]);
    buf[len] = '\0';
    /* wc -l should output 3 (with possible leading whitespace) */
    assert(strstr(buf, "3") != NULL);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* ── 3. Test 3-stage pipeline: cat abc.c | sort | head -5 ───────── */
    printf("Testing 3-stage pipeline (cat file | sort | head -5)... ");
    /* Create an unsorted file /tmp/abc.c */
    FILE *fabc = fopen("/tmp/abc.c", "w");
    assert(fabc != NULL);
    fprintf(fabc, "zebra\nyellow\napple\norange\nbanana\ngrape\ncherry\n");
    fclose(fabc);

    num = lexer_tokenize("cat abc.c | sort | head -5", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 3);

    assert(pipe(pfd) == 0);
    status = pipeline_exec_pipeline(ctx, &pl, pfd[1]);
    close(pfd[1]);
    assert(status == 0);

    assert(proto_recv(pfd[0], &type, buf, sizeof(buf) - 1, &len) == 0);
    close(pfd[0]);
    buf[len] = '\0';
    /* Output should be first 5 sorted words */
    assert(strcmp(buf, "apple\nbanana\ncherry\ngrape\norange\n") == 0);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* ── 4. Pipeline combined with input redirection (< in | cmd) ───── */
    printf("Testing pipeline with input redirection (sort < abc.c | head -3)... ");
    num = lexer_tokenize("sort < abc.c | head -3", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 2);
    assert(pl.commands[0].infile != NULL && strcmp(pl.commands[0].infile, "abc.c") == 0);

    assert(pipe(pfd) == 0);
    status = pipeline_exec_pipeline(ctx, &pl, pfd[1]);
    close(pfd[1]);
    assert(status == 0);

    assert(proto_recv(pfd[0], &type, buf, sizeof(buf) - 1, &len) == 0);
    close(pfd[0]);
    buf[len] = '\0';
    assert(strcmp(buf, "apple\nbanana\ncherry\n") == 0);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* ── 5. Pipeline combined with output redirection (cmd | cmd > out) ─ */
    printf("Testing pipeline with output redirection (cat abc.c | head -2 > out.txt)... ");
    unlink("/tmp/out.txt");
    num = lexer_tokenize("cat abc.c | head -2 > out.txt", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 2);
    assert(pl.commands[1].outfile != NULL && strcmp(pl.commands[1].outfile, "out.txt") == 0);

    status = pipeline_exec_pipeline(ctx, &pl, -1);
    assert(status == 0);

    FILE *fout = fopen("/tmp/out.txt", "r");
    assert(fout != NULL);
    char out_content[256] = {0};
    size_t nr = fread(out_content, 1, sizeof(out_content) - 1, fout);
    fclose(fout);
    assert(nr > 0);
    assert(strcmp(out_content, "zebra\nyellow\n") == 0);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* ── 6. Pipeline combined with both < and > ───────────────────────── */
    printf("Testing pipeline with both < and > (sort < abc.c | head -2 > out2.txt)... ");
    unlink("/tmp/out2.txt");
    num = lexer_tokenize("sort < abc.c | head -2 > out2.txt", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 2);
    assert(pl.commands[0].infile != NULL && strcmp(pl.commands[0].infile, "abc.c") == 0);
    assert(pl.commands[1].outfile != NULL && strcmp(pl.commands[1].outfile, "out2.txt") == 0);

    status = pipeline_exec_pipeline(ctx, &pl, -1);
    assert(status == 0);

    fout = fopen("/tmp/out2.txt", "r");
    assert(fout != NULL);
    memset(out_content, 0, sizeof(out_content));
    nr = fread(out_content, 1, sizeof(out_content) - 1, fout);
    fclose(fout);
    assert(nr > 0);
    assert(strcmp(out_content, "apple\nbanana\n") == 0);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* ── 7. Pipeline exit status of last command ─────────────────────── */
    printf("Testing pipeline exit status propagates last command... ");
    num = lexer_tokenize("echo hello | grep missing_term_xyz", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) == 0);
    status = pipeline_exec_pipeline(ctx, &pl, -1);
    assert(status == 1);
    assert(ctx->last_status == 1);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* Clean up temporary test files */
    unlink("/tmp/test1.txt");
    unlink("/tmp/test2.txt");
    unlink("/tmp/test3.txt");
    unlink("/tmp/abc.c");
    unlink("/tmp/out.txt");
    unlink("/tmp/out2.txt");

    shell_ctx_destroy(ctx);
    printf("=== All Pipeline & Globbing Unit Tests Passed! ===\n\n");
    return 0;
}
