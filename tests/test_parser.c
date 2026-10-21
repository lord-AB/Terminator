/*
 * test_parser.c — Unit tests for multiline detection and quoting rules
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "parser/multiline.h"

int main(void) {
    printf("\n=== Running Multiline & Quoting Unit Tests ===\n");

    /* 1. Complete single-line commands */
    printf("Testing complete commands... ");
    assert(multiline_is_incomplete("ls") == 0);
    assert(multiline_is_incomplete("ls -la /tmp") == 0);
    assert(multiline_is_incomplete("echo 'hello world'") == 0);
    assert(multiline_is_incomplete("echo \"hello world\"") == 0);
    assert(multiline_is_incomplete("echo \"escaped \\\" quote\"") == 0);
    assert(multiline_is_incomplete("echo $(date +%T)") == 0);
    assert(multiline_is_incomplete("echo `date +%T`") == 0);
    assert(multiline_is_incomplete("echo \\\\") == 0); /* double backslash is escaped */
    printf("PASS\n");

    /* 2. Trailing backslash line continuation */
    printf("Testing trailing backslash continuation... ");
    assert(multiline_is_incomplete("echo hello \\") == 1);
    assert(multiline_is_incomplete("echo hello \\  ") == 1); /* with trailing spaces */
    assert(multiline_is_incomplete("ls | \\") == 1);
    assert(multiline_is_incomplete("echo \"hello \\\" \\") == 1);
    printf("PASS\n");

    /* 3. Unclosed quotes */
    printf("Testing unclosed single and double quotes... ");
    assert(multiline_is_incomplete("echo 'hello") == 1);
    assert(multiline_is_incomplete("echo \"hello") == 1);
    assert(multiline_is_incomplete("echo 'hello \\") == 1); /* 1 single quote opened */
    assert(multiline_is_incomplete("echo \"hello \\\" world") == 1);
    printf("PASS\n");

    /* 4. Unclosed command substitutions */
    printf("Testing command substitution $(...) and `...`... ");
    assert(multiline_is_incomplete("echo $(basename") == 1);
    assert(multiline_is_incomplete("echo $(ls | \\") == 1);
    assert(multiline_is_incomplete("echo `pwd") == 1);
    assert(multiline_is_incomplete("echo \"Files in $(basename $(pwd)): $(ls | \\") == 1);
    printf("PASS\n");

    /* 5. Design.md exact examples */
    printf("Testing design.md spec examples... ");
    /* Example 1 line 1: echo "Isn't process creation exciting?\ */
    assert(multiline_is_incomplete("echo \"Isn't process creation exciting?\\") == 1);
    /* Example 1 combined: echo "Isn't process creation exciting? Fork, exec, repeat!" */
    assert(multiline_is_incomplete("echo \"Isn't process creation exciting? Fork, exec, repeat!\"") == 0);

    /* Example 2 line 1: echo "Files in $(basename $(pwd)): $(ls | \ */
    assert(multiline_is_incomplete("echo \"Files in $(basename $(pwd)): $(ls | \\") == 1);
    /* Example 2 combined: echo "Files in $(basename $(pwd)): $(ls | \n wc -l)" */
    assert(multiline_is_incomplete("echo \"Files in $(basename $(pwd)): $(ls | \n wc -l)\"") == 0);

    printf("PASS\n");

    printf("=== All Multiline Unit Tests Passed! ===\n\n");
    return 0;
}
