/*
 * verification.h — Headless integration verification suite
 */
#ifndef VERIFICATION_H
#define VERIFICATION_H

#include "common.h"

/*
 * Runs end-to-end integration tests (CWD isolation, builtins, external
 * commands, multiline/quoting, and I/O redirection) in headless mode.
 * Returns 0 on success, non-zero on failure.
 */
int run_verification_test(ExecMode mode);

#endif /* VERIFICATION_H */
