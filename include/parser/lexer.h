/*
 * lexer.h — Quote-aware tokenizer for words and operators
 */
#ifndef PARSER_LEXER_H
#define PARSER_LEXER_H

#include <stddef.h>
#include "common.h"

/*
 * Tokenizes the command string `input` into an array of arguments / operators.
 *
 * Rules:
 * - Unquoted whitespace separates words.
 * - Single quotes ('...') and double quotes ("...") preserve whitespace within them.
 * - Syntactic quote characters are stripped from the resulting tokens.
 * - Operators ('|', '<', '>', '&', '>>') outside quotes are tokenized as distinct tokens.
 * - Empty quotes ("" or '') produce an empty string token.
 *
 * Populates `tokens` array up to `max_tokens - 1`.
 * Terminates array with tokens[argc] = NULL.
 * Returns the number of tokens (argc).
 *
 * Each token string is allocated on the heap; caller frees with `lexer_free_tokens()`.
 */
int lexer_tokenize(const char *input, char *tokens[], int max_tokens);

/*
 * Frees all token strings allocated in `tokens`.
 */
void lexer_free_tokens(char *tokens[], int count);

#endif /* PARSER_LEXER_H */
