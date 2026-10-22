/*
 * ast.h — Command and Pipeline Abstract Syntax Tree structures
 */
#ifndef PARSER_AST_H
#define PARSER_AST_H

#include <stddef.h>
#include "common.h"

#define MAX_COMMANDS_IN_PIPELINE 16

/*
 * Represents a single command segment within a pipeline.
 * Holds its argument vector (argv), input redirection file,
 * output redirection file, and append mode flag.
 */
typedef struct Command {
    char *argv[MAX_ARGS];
    int   argc;
    char *infile;       /* Input redirection file (< file), or NULL */
    char *outfile;      /* Output redirection file (> file or >> file), or NULL */
    int   append_out;   /* 1 for >> (append), 0 for > (truncate) */
} Command;

/*
 * Represents a pipeline of one or more commands connected by pipes (|).
 * Optionally marked as a background job (&).
 */
typedef struct Pipeline {
    Command commands[MAX_COMMANDS_IN_PIPELINE];
    int     num_commands;
    int     is_background;  /* 1 if command ended with '&' */
    char    cmd_str[MAX_CMD_LEN];
} Pipeline;

/*
 * Initialize a Command struct to empty.
 */
void command_init(Command *cmd);

/*
 * Free dynamically allocated strings inside a Command struct (argv, infile, outfile).
 */
void command_free(Command *cmd);

/*
 * Initialize a Pipeline struct to empty.
 */
void pipeline_init(Pipeline *pl);

/*
 * Free all commands and strings inside a Pipeline struct.
 */
void pipeline_free(Pipeline *pl);

/*
 * Parses tokens (from lexer_tokenize) into a Pipeline AST.
 * Extracts:
 * - Redirection tokens '< infile', '> outfile', '>> outfile'
 * - Pipe tokens '|' separating commands
 * - Background token '&'
 *
 * Populates `pl`.
 * Returns 0 on success, -1 on syntax error (e.g., missing file after redirection).
 */
int ast_parse(char *tokens[], int num_tokens, Pipeline *pl);

#endif /* PARSER_AST_H */
