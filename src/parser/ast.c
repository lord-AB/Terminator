/*
 * ast.c — Builds and frees Command and Pipeline structures from tokens
 */
#include "parser/ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void command_init(Command *cmd) {
    if (!cmd) return;
    memset(cmd, 0, sizeof(Command));
}

void command_free(Command *cmd) {
    if (!cmd) return;
    for (int i = 0; i < cmd->argc; i++) {
        if (cmd->argv[i]) {
            free(cmd->argv[i]);
            cmd->argv[i] = NULL;
        }
    }
    cmd->argc = 0;

    if (cmd->infile) {
        free(cmd->infile);
        cmd->infile = NULL;
    }
    if (cmd->outfile) {
        free(cmd->outfile);
        cmd->outfile = NULL;
    }
    cmd->append_out = 0;
}

void pipeline_init(Pipeline *pl) {
    if (!pl) return;
    memset(pl, 0, sizeof(Pipeline));
    for (int i = 0; i < MAX_COMMANDS_IN_PIPELINE; i++) {
        command_init(&pl->commands[i]);
    }
    pl->num_commands = 0;
    pl->is_background = 0;
}

void pipeline_free(Pipeline *pl) {
    if (!pl) return;
    for (int i = 0; i < pl->num_commands; i++) {
        command_free(&pl->commands[i]);
    }
    pl->num_commands = 0;
    pl->is_background = 0;
}

static int is_operator_token(const char *token) {
    if (!token) return 0;
    return (strcmp(token, "|") == 0 ||
            strcmp(token, "<") == 0 ||
            strcmp(token, ">") == 0 ||
            strcmp(token, ">>") == 0 ||
            strcmp(token, "&") == 0);
}

int ast_parse(char *tokens[], int num_tokens, Pipeline *pl) {
    if (!tokens || num_tokens <= 0 || !pl) return -1;

    pipeline_init(pl);

    int cur_cmd_idx = 0;
    Command *cur_cmd = &pl->commands[cur_cmd_idx];
    pl->num_commands = 1;

    for (int i = 0; i < num_tokens; i++) {
        const char *t = tokens[i];
        if (!t) continue;

        /* 1. Pipe operator '|' */
        if (strcmp(t, "|") == 0) {
            /* Pipe must be preceded by a valid command or redirection */
            if (cur_cmd->argc == 0 && !cur_cmd->infile && !cur_cmd->outfile) {
                pipeline_free(pl);
                return -1;
            }

            cur_cmd->argv[cur_cmd->argc] = NULL;
            cur_cmd_idx++;
            if (cur_cmd_idx >= MAX_COMMANDS_IN_PIPELINE) {
                pipeline_free(pl);
                return -1;
            }
            cur_cmd = &pl->commands[cur_cmd_idx];
            pl->num_commands++;
            continue;
        }

        /* 2. Background operator '&' */
        if (strcmp(t, "&") == 0) {
            /* Background operator should be at the end */
            if (i == num_tokens - 1) {
                pl->is_background = 1;
                break;
            } else {
                /* If '&' is in the middle of a command without job chaining */
                pl->is_background = 1;
                continue;
            }
        }

        /* 3. Input redirection '<' */
        if (strcmp(t, "<") == 0) {
            if (i + 1 >= num_tokens || is_operator_token(tokens[i + 1])) {
                pipeline_free(pl);
                return -1; /* Missing filename after '<' */
            }
            if (cur_cmd->infile) {
                free(cur_cmd->infile);
            }
            cur_cmd->infile = strdup(tokens[i + 1]);
            i++; /* consume filename */
            continue;
        }

        /* 4. Output redirection '>' or '>>' */
        if (strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {
            int is_append = (strcmp(t, ">>") == 0);
            if (i + 1 >= num_tokens || is_operator_token(tokens[i + 1])) {
                pipeline_free(pl);
                return -1; /* Missing filename after '>' */
            }
            if (cur_cmd->outfile) {
                free(cur_cmd->outfile);
            }
            cur_cmd->outfile = strdup(tokens[i + 1]);
            cur_cmd->append_out = is_append;
            i++; /* consume filename */
            continue;
        }

        /* 5. Regular command argument */
        if (cur_cmd->argc < MAX_ARGS - 1) {
            cur_cmd->argv[cur_cmd->argc++] = strdup(t);
        }
    }

    /* Finalize the last command segment */
    cur_cmd->argv[cur_cmd->argc] = NULL;

    /* If the last command was empty after a pipe (e.g., "cmd |") */
    if (cur_cmd->argc == 0 && !cur_cmd->infile && !cur_cmd->outfile && cur_cmd_idx > 0) {
        pipeline_free(pl);
        return -1;
    }

    return 0;
}
