/*
 * test_parser.c — Unit tests for multiline detection, variable expansion,
 *                  command substitution, and quote-aware lexing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "parser/multiline.h"
#include "parser/expand.h"
#include "parser/subst.h"
#include "parser/lexer.h"
#include "exec/shell_ctx.h"

int main(void) {
    printf("\n=== Running Multiline, Quoting & Substitution Unit Tests ===\n");

    /* 1. Complete single-line commands */
    printf("Testing complete commands... ");
    assert(multiline_is_incomplete("ls") == 0);
    assert(multiline_is_incomplete("ls -la /tmp") == 0);
    assert(multiline_is_incomplete("echo 'hello world'") == 0);
    assert(multiline_is_incomplete("echo \"hello world\"") == 0);
    assert(multiline_is_incomplete("echo \"escaped \\\" quote\"") == 0);
    assert(multiline_is_incomplete("echo $(date +%T)") == 0);
    assert(multiline_is_incomplete("echo `date +%T`") == 0);
    assert(multiline_is_incomplete("echo \\\\") == 0);
    printf("PASS\n");

    /* 2. Trailing backslash line continuation */
    printf("Testing trailing backslash continuation... ");
    assert(multiline_is_incomplete("echo hello \\") == 1);
    assert(multiline_is_incomplete("echo hello \\  ") == 1);
    assert(multiline_is_incomplete("ls | \\") == 1);
    assert(multiline_is_incomplete("echo \"hello \\\" \\") == 1);
    printf("PASS\n");

    /* 3. Unclosed quotes */
    printf("Testing unclosed single and double quotes... ");
    assert(multiline_is_incomplete("echo 'hello") == 1);
    assert(multiline_is_incomplete("echo \"hello") == 1);
    assert(multiline_is_incomplete("echo 'hello \\") == 1);
    assert(multiline_is_incomplete("echo \"hello \\\" world") == 1);
    printf("PASS\n");

    /* 4. Unclosed command substitutions */
    printf("Testing unclosed $(...) and `...`... ");
    assert(multiline_is_incomplete("echo $(basename") == 1);
    assert(multiline_is_incomplete("echo $(ls | \\") == 1);
    assert(multiline_is_incomplete("echo `pwd") == 1);
    assert(multiline_is_incomplete("echo \"Files in $(basename $(pwd)): $(ls | \\") == 1);
    printf("PASS\n");

    /* 5. Variable expansion ($VAR, $?, ${VAR}) and line continuation (\\\n) */
    printf("Testing variable expansion & line continuations... ");
    ShellContext *ctx = shell_ctx_create(0);
    ctx->last_status = 42;
    shell_ctx_set_var(ctx, "MYVAR", "terminator_val");

    char out[MAX_CMD_LEN];
    /* $? expansion */
    expand_variables(ctx, "exit code was $?", out, sizeof(out));
    assert(strcmp(out, "exit code was 42") == 0);

    /* Local variable expansion */
    expand_variables(ctx, "val is $MYVAR", out, sizeof(out));
    assert(strcmp(out, "val is terminator_val") == 0);

    /* ${VAR} syntax */
    expand_variables(ctx, "val is ${MYVAR}_suffix", out, sizeof(out));
    assert(strcmp(out, "val is terminator_val_suffix") == 0);

    /* Single quotes are strictly literal */
    expand_variables(ctx, "echo 'literal: $MYVAR $?'", out, sizeof(out));
    assert(strcmp(out, "echo 'literal: $MYVAR $?'") == 0);

    /* Double quotes expand */
    expand_variables(ctx, "echo \"expanded: $MYVAR $?\"", out, sizeof(out));
    assert(strcmp(out, "echo \"expanded: terminator_val 42\"") == 0);

    /* Line continuation \\\n removal */
    expand_variables(ctx, "line 1 \\\nline 2", out, sizeof(out));
    assert(strcmp(out, "line 1 line 2") == 0);
    printf("PASS\n");

    /* 6. Command substitution (innermost-first $(...) and `...`) */
    printf("Testing command substitution... ");
    /* Simple $(echo hello) */
    subst_expand(ctx, "msg: $(echo hello)", out, sizeof(out));
    assert(strcmp(out, "msg: hello") == 0);

    /* Backticks `echo world` */
    subst_expand(ctx, "msg: `echo world`", out, sizeof(out));
    assert(strcmp(out, "msg: world") == 0);

    /* Nested substitutions: innermost $(echo foo) runs first */
    subst_expand(ctx, "result: $(echo nested $(echo foo))", out, sizeof(out));
    assert(strcmp(out, "result: nested foo") == 0);

    /* Substitutions inside single quotes must NOT expand */
    subst_expand(ctx, "echo 'literal $(echo bar)'", out, sizeof(out));
    assert(strcmp(out, "echo 'literal $(echo bar)'") == 0);

    /* Substitutions inside double quotes MUST expand */
    subst_expand(ctx, "echo \"expanded $(echo bar)\"", out, sizeof(out));
    assert(strcmp(out, "echo \"expanded bar\"") == 0);
    printf("PASS\n");

    /* 7. Quote-aware lexer / tokenizer */
    printf("Testing quote-aware lexer... ");
    char *tokens[MAX_ARGS];
    int count = lexer_tokenize("echo 'literal: $HOME' \"expanded: /test\"", tokens, MAX_ARGS);
    assert(count == 3);
    assert(strcmp(tokens[0], "echo") == 0);
    assert(strcmp(tokens[1], "literal: $HOME") == 0);
    assert(strcmp(tokens[2], "expanded: /test") == 0);
    lexer_free_tokens(tokens, count);

    /* Operator separation */
    count = lexer_tokenize("cat < in.txt | grep foo > out.txt", tokens, MAX_ARGS);
    assert(count == 8);
    assert(strcmp(tokens[0], "cat") == 0);
    assert(strcmp(tokens[1], "<") == 0);
    assert(strcmp(tokens[2], "in.txt") == 0);
    assert(strcmp(tokens[3], "|") == 0);
    assert(strcmp(tokens[4], "grep") == 0);
    assert(strcmp(tokens[5], "foo") == 0);
    assert(strcmp(tokens[6], ">") == 0);
    assert(strcmp(tokens[7], "out.txt") == 0);
    lexer_free_tokens(tokens, count);

    /* Operators without spaces */
    count = lexer_tokenize("ls|wc>>count.log", tokens, MAX_ARGS);
    assert(count == 5);
    assert(strcmp(tokens[0], "ls") == 0);
    assert(strcmp(tokens[1], "|") == 0);
    assert(strcmp(tokens[2], "wc") == 0);
    assert(strcmp(tokens[3], ">>") == 0);
    assert(strcmp(tokens[4], "count.log") == 0);
    lexer_free_tokens(tokens, count);

    /* Preserving quoted operators */
    count = lexer_tokenize("echo \"a | b > c\"", tokens, MAX_ARGS);
    assert(count == 2);
    assert(strcmp(tokens[0], "echo") == 0);
    assert(strcmp(tokens[1], "a | b > c") == 0);
    lexer_free_tokens(tokens, count);
    printf("PASS\n");

    /* 8. Design.md exact examples */
    printf("Testing design.md spec examples... ");

    /* Example 1: Multiline with line continuation */
    const char *ex1_raw = "echo \"Isn't process creation exciting?\\\nFork, exec, repeat!\"";
    assert(multiline_is_incomplete("echo \"Isn't process creation exciting?\\") == 1);
    assert(multiline_is_incomplete(ex1_raw) == 0);

    char ex1_subst[MAX_CMD_LEN];
    subst_expand(ctx, ex1_raw, ex1_subst, sizeof(ex1_subst));
    char ex1_exp[MAX_CMD_LEN];
    expand_variables(ctx, ex1_subst, ex1_exp, sizeof(ex1_exp));
    count = lexer_tokenize(ex1_exp, tokens, MAX_ARGS);
    assert(count == 2);
    assert(strcmp(tokens[0], "echo") == 0);
    assert(strcmp(tokens[1], "Isn't process creation exciting?Fork, exec, repeat!") == 0);
    lexer_free_tokens(tokens, count);

    /* Example 2: Nested command substitution and pipeline */
    /* echo "Files in $(basename $(pwd)): $(ls | wc -l)" */
    strncpy(ctx->cwd, "/tmp", sizeof(ctx->cwd));
    const char *ex2_raw = "echo \"Files in $(basename $(pwd)): $(ls | wc -l)\"";
    char ex2_subst[MAX_CMD_LEN];
    subst_expand(ctx, ex2_raw, ex2_subst, sizeof(ex2_subst));
    char ex2_exp[MAX_CMD_LEN];
    expand_variables(ctx, ex2_subst, ex2_exp, sizeof(ex2_exp));
    count = lexer_tokenize(ex2_exp, tokens, MAX_ARGS);
    assert(count == 2);
    assert(strcmp(tokens[0], "echo") == 0);
    assert(strncmp(tokens[1], "Files in tmp: ", 14) == 0);
    lexer_free_tokens(tokens, count);

    /* Example 3: bash quoting rules */
    /* echo 'literal: $HOME' "expanded: $HOME" */
    const char *ex3_raw = "echo 'literal: $HOME' \"expanded: $HOME\"";
    char ex3_subst[MAX_CMD_LEN];
    subst_expand(ctx, ex3_raw, ex3_subst, sizeof(ex3_subst));
    char ex3_exp[MAX_CMD_LEN];
    expand_variables(ctx, ex3_subst, ex3_exp, sizeof(ex3_exp));
    count = lexer_tokenize(ex3_exp, tokens, MAX_ARGS);
    assert(count == 3);
    assert(strcmp(tokens[0], "echo") == 0);
    assert(strcmp(tokens[1], "literal: $HOME") == 0);
    assert(strstr(tokens[2], "expanded: ") != NULL);
    assert(strstr(tokens[2], "$HOME") == NULL); /* $HOME was expanded! */
    lexer_free_tokens(tokens, count);

    shell_ctx_destroy(ctx);
    printf("PASS\n");

    printf("=== All Multiline & Quoting Unit Tests Passed! ===\n\n");
    return 0;
}
