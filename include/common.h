/*
 * common.h — Global constants, error codes, shared macros for Terminator
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ── Execution mode ─────────────────────────────────────────────────── */
typedef enum {
    MODE_THREAD,    /* default: each tab is a pthread               */
    MODE_PROCESS    /* useProcess=True: each tab is a fork()ed child */
} ExecMode;

/* ── Limits ─────────────────────────────────────────────────────────── */
#define MAX_TABS            16
#define HISTORY_CAPACITY    10000
#define HISTORY_DISPLAY     1000
#define MAX_CMD_LEN         4096
#define MAX_LINE_LEN        4096
#define MAX_PATH_LEN        4096
#define MAX_ARGS            256
#define PIPE_BUF_SIZE       4096

/* ── Tab states ─────────────────────────────────────────────────────── */
typedef enum {
    TAB_UNUSED = 0,
    TAB_RUNNING,
    TAB_STOPPED
} TabState;

/* ── Error codes ────────────────────────────────────────────────────── */
#define TERM_OK              0
#define TERM_ERR_ALLOC      -1
#define TERM_ERR_PIPE       -2
#define TERM_ERR_FORK       -3
#define TERM_ERR_SHM        -4
#define TERM_ERR_SYNC       -5
#define TERM_ERR_PROTOCOL   -6

/* ── Utility macros ─────────────────────────────────────────────────── */
#define TERM_LOG(fmt, ...) \
    fprintf(stderr, "[terminator] " fmt "\n", ##__VA_ARGS__)

#define TERM_DIE(fmt, ...) do {                   \
    fprintf(stderr, "[terminator FATAL] " fmt "\n", ##__VA_ARGS__); \
    exit(EXIT_FAILURE);                            \
} while (0)

#define UNUSED(x) ((void)(x))

/* ── Safe close helper ──────────────────────────────────────────────── */
static inline void safe_close(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

#endif /* COMMON_H */
