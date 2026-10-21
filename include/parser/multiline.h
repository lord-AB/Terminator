/*
 * multiline.h — Incomplete input detector for Terminator
 *
 * Detects whether a command line is incomplete and requires continuation ('> ' prompt).
 * Checks for:
 * - Trailing backslash ('\') for line continuation
 * - Unclosed single quotes ('...')
 * - Unclosed double quotes ("...")
 * - Unclosed command substitutions: $(...) or `...`
 */
#ifndef PARSER_MULTILINE_H
#define PARSER_MULTILINE_H

/*
 * Returns 1 if `input` is incomplete and requires a continuation prompt ('> '),
 * returns 0 if `input` is a complete command ready for execution.
 */
int multiline_is_incomplete(const char *input);

#endif /* PARSER_MULTILINE_H */
