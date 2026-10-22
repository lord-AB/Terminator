/*
 * script_validator.h — Pre-execution syntax checker for bash scripts
 */
#ifndef SCRIPT_SCRIPT_VALIDATOR_H
#define SCRIPT_SCRIPT_VALIDATOR_H

#include "common.h"

#define MAX_VALIDATOR_ERR 256

typedef struct {
    int  is_valid;
    int  error_line;
    char error_msg[MAX_VALIDATOR_ERR];
} ScriptValidationResult;

/*
 * Validates a script file for syntax errors before execution:
 * - Checks balanced single and double quotes.
 * - Checks valid, balanced if / then / elif / else / fi blocks.
 * Returns 0 if syntax is valid, -1 if syntax error (with details in out_res).
 */
int script_validator_check_file(const char *filepath, ScriptValidationResult *out_res);

/*
 * Validates in-memory script text buffer.
 * Returns 0 if syntax is valid, -1 if syntax error.
 */
int script_validator_check_text(const char *text, ScriptValidationResult *out_res);

#endif /* SCRIPT_SCRIPT_VALIDATOR_H */
