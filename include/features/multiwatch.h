/*
 * multiwatch.h — multiWatch command execution and multiplexing
 */
#ifndef FEATURES_MULTIWATCH_H
#define FEATURES_MULTIWATCH_H

#include "common.h"
#include "exec/shell_ctx.h"
#include <sys/types.h>
#include <time.h>

#define MULTIWATCH_MAX_CMDS 16
#define MULTIWATCH_INTERVAL_SEC 2
#define MULTIWATCH_POLL_TIMEOUT_MS 100

typedef struct {
    char   cmd[MAX_CMD_LEN];
    pid_t  pid;
    int    pipe_fd;              /* read end of pipe from child */
    int    running;              /* 1 if process is currently running, 0 otherwise */
    time_t last_finish_time;     /* timestamp when child exited/finished */
} MultiwatchCommand;

/*
 * Parse arguments for multiWatch.
 * Handles:
 *   multiWatch ["cmd1", "cmd2", ...]
 *   multiWatch [ "cmd1" , "cmd2" ]
 *   multiWatch "cmd1" "cmd2"
 *
 * Populates `cmds` array with dynamically allocated strings (caller frees).
 * Returns number of parsed commands, or -1 on syntax error.
 */
int multiwatch_parse_args(int argc, char *argv[], char *cmds[], int max_cmds);

/*
 * Free parsed command strings array.
 */
void multiwatch_free_cmds(char *cmds[], int count);

/*
 * Main execution loop for multiWatch:
 * Runs all commands concurrently in separate processes (setpgid(0,0)),
 * multiplexes their outputs via poll() without busy-waiting,
 * formats output with timestamps and dashed dividers,
 * re-runs commands every 2 seconds after completion,
 * and terminates all children on Ctrl+C (from in_fd or signal).
 *
 * Returns 0 on normal finish or 130 on Ctrl+C.
 */
int multiwatch_run(ShellContext *ctx, int num_cmds, char *cmds[], int out_fd, int in_fd);

#endif /* FEATURES_MULTIWATCH_H */
