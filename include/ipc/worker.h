/*
 * worker.h — Unified interface for spawning tab workers
 *
 * Each tab worker runs a shell loop. In Thread mode, workers are pthreads;
 * in Process mode, workers are fork()ed child processes. Communication
 * between the GUI and each worker happens through two unidirectional pipes:
 *   gui_to_tab: GUI writes commands, tab reads them
 *   tab_to_gui: Tab writes output, GUI reads it
 */
#ifndef IPC_WORKER_H
#define IPC_WORKER_H

#include <pthread.h>
#include <sys/types.h>
#include "common.h"
#include "ipc/shm_sync.h"

/* ── Per-tab worker handle ──────────────────────────────────────────── */
typedef struct {
    int         tab_id;             /* index in the tab array (0-based)    */
    TabState    state;              /* UNUSED, RUNNING, STOPPED            */

    /* Pipe file descriptors (from GUI's perspective):
     *   gui_write_fd  → GUI writes here, tab reads from gui_read_fd
     *   gui_read_fd   → GUI reads here, tab writes to tab_write_fd
     */
    int         gui_write_fd;       /* GUI → Tab pipe: write end           */
    int         gui_read_fd;        /* Tab → GUI pipe: read end            */

    /* Internal pipe ends (held by the worker, closed in GUI after fork) */
    int         tab_read_fd;        /* GUI → Tab pipe: read end            */
    int         tab_write_fd;       /* Tab → GUI pipe: write end           */

    /* Worker identity */
    pthread_t   thread_id;          /* valid only in MODE_THREAD           */
    pid_t       child_pid;          /* valid only in MODE_PROCESS          */

    /* Shared state pointer (passed to worker for history access) */
    SharedState *shared;
    ExecMode    mode;
} TabWorker;

/* ── Arguments passed to the tab worker entry function ──────────────── */
typedef struct {
    int          tab_id;
    int          read_fd;           /* pipe to read commands from GUI      */
    int          write_fd;          /* pipe to write output to GUI         */
    SharedState *shared;
    ExecMode     mode;
} WorkerArgs;

/*
 * Initialize a TabWorker struct (zero out, set fds to -1).
 */
void worker_init(TabWorker *w);

/*
 * Spawn a new tab worker. Creates two pipes and either a thread or a
 * forked process depending on `mode`.
 * Returns 0 on success, negative error code on failure.
 */
int worker_spawn(TabWorker *w, int tab_id, ExecMode mode, SharedState *shared);

/*
 * Stop a tab worker: send a close message, then join/waitpid.
 * Returns 0 on success.
 */
int worker_stop(TabWorker *w);

/*
 * The tab worker entry point (runs the shell loop).
 * For threads: this is the pthread start routine.
 * For processes: this is called after fork() in the child.
 */
void *worker_entry(void *arg);

#endif /* IPC_WORKER_H */
