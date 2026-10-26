/*
 * test_redirect.c — Unit and functional tests for AST parsing and I/O Redirection
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#include "parser/lexer.h"
#include "parser/ast.h"
#include "exec/redirect.h"
#include "exec/pipeline.h"
#include "exec/shell_ctx.h"
#include "ipc/protocol.h"

int main(void) {
    printf("\n=== Running AST & Redirection Unit Tests ===\n");

    /* ── 1. AST Parsing of Redirections ──────────────────────────────── */
    printf("Testing AST parser with redirections... ");
    char *tokens[MAX_ARGS];

    /* 1a. sort < somefile.txt */
    int num = lexer_tokenize("sort < somefile.txt", tokens, MAX_ARGS);
    assert(num == 3);
    Pipeline pl;
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 1);
    assert(pl.commands[0].argc == 1);
    assert(strcmp(pl.commands[0].argv[0], "sort") == 0);
    assert(pl.commands[0].infile != NULL && strcmp(pl.commands[0].infile, "somefile.txt") == 0);
    assert(pl.commands[0].outfile == NULL);
    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);

    /* 1b. ls > abc */
    num = lexer_tokenize("ls > abc", tokens, MAX_ARGS);
    assert(num == 3);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 1);
    assert(pl.commands[0].argc == 1);
    assert(strcmp(pl.commands[0].argv[0], "ls") == 0);
    assert(pl.commands[0].infile == NULL);
    assert(pl.commands[0].outfile != NULL && strcmp(pl.commands[0].outfile, "abc") == 0);
    assert(pl.commands[0].append_out == 0);
    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);

    /* 1c. ./a.out < infile.txt > outfile.txt */
    num = lexer_tokenize("./a.out < infile.txt > outfile.txt", tokens, MAX_ARGS);
    assert(num == 5);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 1);
    assert(pl.commands[0].argc == 1);
    assert(strcmp(pl.commands[0].argv[0], "./a.out") == 0);
    assert(pl.commands[0].infile != NULL && strcmp(pl.commands[0].infile, "infile.txt") == 0);
    assert(pl.commands[0].outfile != NULL && strcmp(pl.commands[0].outfile, "outfile.txt") == 0);
    assert(pl.commands[0].append_out == 0);
    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);

    /* 1d. < missing_file.txt (redirection only) */
    num = lexer_tokenize("< missing_file.txt", tokens, MAX_ARGS);
    assert(num == 2);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.num_commands == 1);
    assert(pl.commands[0].argc == 0);
    assert(pl.commands[0].argv[0] == NULL);
    assert(pl.commands[0].infile != NULL && strcmp(pl.commands[0].infile, "missing_file.txt") == 0);
    assert(pl.commands[0].outfile == NULL);
    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);

    /* 1e. Append mode >> */
    num = lexer_tokenize("echo test >> log.txt", tokens, MAX_ARGS);
    assert(num == 4);
    assert(ast_parse(tokens, num, &pl) == 0);
    assert(pl.commands[0].append_out == 1);
    assert(strcmp(pl.commands[0].outfile, "log.txt") == 0);
    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);

    /* 1f. Syntax errors (missing filename) */
    num = lexer_tokenize("sort <", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) != 0);
    lexer_free_tokens(tokens, num);

    num = lexer_tokenize("ls >", tokens, MAX_ARGS);
    assert(ast_parse(tokens, num, &pl) != 0);
    lexer_free_tokens(tokens, num);

    printf("PASS\n");

    /* ── 2. Functional Execution of Redirections ─────────────────────── */
    printf("Testing execution with input redirection (sort < file)... ");
    ShellContext *ctx = shell_ctx_create(0);
    strncpy(ctx->cwd, "/tmp", sizeof(ctx->cwd));

    /* Create test file /tmp/test_sort_in.txt with unsorted numbers */
    FILE *fp = fopen("/tmp/test_sort_in.txt", "w");
    assert(fp != NULL);
    fprintf(fp, "zebra\napple\nmango\nbanana\n");
    fclose(fp);

    /* Execute sort < test_sort_in.txt */
    num = lexer_tokenize("sort < test_sort_in.txt", tokens, MAX_ARGS);
    ast_parse(tokens, num, &pl);

    int pfd[2];
    assert(pipe(pfd) == 0);
    int status = pipeline_exec_cmd(ctx, &pl.commands[0], pfd[1]);
    close(pfd[1]);
    assert(status == 0);

    /* Read captured stdout using protocol */
    char buf[1024];
    uint32_t type, len;
    assert(proto_recv(pfd[0], &type, buf, sizeof(buf) - 1, &len) == 0);
    close(pfd[0]);
    assert(type == TAB_MSG_OUTPUT);
    buf[len] = '\0';
    assert(strcmp(buf, "apple\nbanana\nmango\nzebra\n") == 0);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* 2b. Output redirection (ls > abc) */
    printf("Testing execution with output redirection (echo > file)... ");
    unlink("/tmp/test_out_abc.txt");

    num = lexer_tokenize("echo \"terminator output\" > test_out_abc.txt", tokens, MAX_ARGS);
    ast_parse(tokens, num, &pl);
    status = pipeline_exec_cmd(ctx, &pl.commands[0], -1);
    assert(status == 0);

    /* Verify file contents */
    fp = fopen("/tmp/test_out_abc.txt", "r");
    assert(fp != NULL);
    char line[128];
    assert(fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);
    assert(strstr(line, "terminator output") != NULL);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* 2c. Combined input & output redirection (./a.out < in.txt > out.txt) */
    printf("Testing combined redirection (< in > out)... ");
    /* Compile a small helper in /tmp */
    fp = fopen("/tmp/redir_helper.c", "w");
    assert(fp != NULL);
    fprintf(fp, "#include <stdio.h>\n"
                "#include <ctype.h>\n"
                "int main() {\n"
                "    int c;\n"
                "    while ((c = getchar()) != EOF) putchar(toupper(c));\n"
                "    return 0;\n"
                "}\n");
    fclose(fp);

    /* Compile redir_helper without invoking system() or a shell */
    pid_t comp_pid = fork();
    if (comp_pid == 0) {
        char *gcc_argv[] = {"gcc", "-o", "/tmp/redir_helper", "/tmp/redir_helper.c", NULL};
        execvp("gcc", gcc_argv);
        _exit(1);
    }
    int comp_status = 0;
    waitpid(comp_pid, &comp_status, 0);

    /* Create input file */
    fp = fopen("/tmp/test_combo_in.txt", "w");
    assert(fp != NULL);
    fprintf(fp, "terminator redirection test\n");
    fclose(fp);
    unlink("/tmp/test_combo_out.txt");

    /* Execute /tmp/redir_helper < test_combo_in.txt > test_combo_out.txt */
    num = lexer_tokenize("./redir_helper < test_combo_in.txt > test_combo_out.txt", tokens, MAX_ARGS);
    ast_parse(tokens, num, &pl);
    status = pipeline_exec_cmd(ctx, &pl.commands[0], -1);
    assert(status == 0);

    /* Verify output file has uppercase text */
    fp = fopen("/tmp/test_combo_out.txt", "r");
    assert(fp != NULL);
    assert(fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);
    assert(strcmp(line, "TERMINATOR REDIRECTION TEST\n") == 0);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* 2d. Missing file error reporting */
    printf("Testing missing file error reporting (< missing.txt)... ");
    unlink("/tmp/nonexistent_file_12345.txt");
    num = lexer_tokenize("< nonexistent_file_12345.txt", tokens, MAX_ARGS);
    ast_parse(tokens, num, &pl);

    assert(pipe(pfd) == 0);
    status = pipeline_exec_cmd(ctx, &pl.commands[0], pfd[1]);
    close(pfd[1]);
    assert(status == 1);
    assert(ctx->last_status == 1);

    assert(proto_recv(pfd[0], &type, buf, sizeof(buf) - 1, &len) == 0);
    close(pfd[0]);
    buf[len] = '\0';
    assert(strstr(buf, "terminator: nonexistent_file_12345.txt: No such file or directory") != NULL);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* 2e. Command with missing file (sort < missing.txt) */
    printf("Testing sort < missing.txt aborts before execution... ");
    num = lexer_tokenize("sort < nonexistent_file_12345.txt", tokens, MAX_ARGS);
    ast_parse(tokens, num, &pl);

    assert(pipe(pfd) == 0);
    status = pipeline_exec_cmd(ctx, &pl.commands[0], pfd[1]);
    close(pfd[1]);
    assert(status == 1);
    assert(ctx->last_status == 1);

    assert(proto_recv(pfd[0], &type, buf, sizeof(buf) - 1, &len) == 0);
    close(pfd[0]);
    buf[len] = '\0';
    assert(strstr(buf, "terminator: nonexistent_file_12345.txt: No such file or directory") != NULL);

    pipeline_free(&pl);
    lexer_free_tokens(tokens, num);
    printf("PASS\n");

    /* Clean up temporary test files */
    unlink("/tmp/test_sort_in.txt");
    unlink("/tmp/test_out_abc.txt");
    unlink("/tmp/redir_helper.c");
    unlink("/tmp/redir_helper");
    unlink("/tmp/test_combo_in.txt");
    unlink("/tmp/test_combo_out.txt");

    shell_ctx_destroy(ctx);

    printf("=== All AST & Redirection Tests Passed! ===\n\n");
    return 0;
}
