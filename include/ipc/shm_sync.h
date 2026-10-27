/*
 * shm_sync.h — POSIX shared memory and process-shared mutex wrappers
 *
 * Provides a shared memory region containing the global history ring buffer
 * and a process-shared pthread_mutex for synchronization. Works in both
 * Thread mode (mutex in process memory) and Process mode (mutex in
 * mmap(MAP_SHARED|MAP_ANONYMOUS) memory with PTHREAD_PROCESS_SHARED).
 */
#ifndef IPC_SHM_SYNC_H
#define IPC_SHM_SYNC_H

#include <pthread.h>
#include <semaphore.h>
#include "common.h"

/* ── History entry ──────────────────────────────────────────────────── */
typedef struct {
    char cmd[MAX_CMD_LEN];      /* the command string                   */
    int  valid;                 /* 1 if this slot holds a real command   */
} HistoryEntry;

/* ── Shared memory layout ───────────────────────────────────────────── */
typedef struct {
    sem_t           sem;                        /* process-shared semaphore (Linux) */
    pthread_mutex_t lock;                       /* process-shared mutex (fallback/thread mode) */
    int             use_sem;                    /* 1 if using POSIX semaphore */
    int             count;                      /* total commands added */
    int             head;                       /* next write index (ring) */
    HistoryEntry    entries[HISTORY_CAPACITY];   /* ring buffer         */
} SharedState;

/*
 * Allocate and initialize the shared memory region.
 * In MODE_PROCESS: uses mmap(MAP_SHARED|MAP_ANONYMOUS).
 * In MODE_THREAD:  uses plain malloc (shared within the process).
 * The mutex is configured with PTHREAD_PROCESS_SHARED when in process mode.
 * Returns a pointer to SharedState, or NULL on failure.
 */
SharedState *shm_create(ExecMode mode);

/*
 * Destroy the shared state (destroy mutex, unmap/free memory).
 */
void shm_destroy(SharedState *state, ExecMode mode);

/*
 * Lock the shared mutex. Returns 0 on success.
 */
int shm_lock(SharedState *state);

/*
 * Unlock the shared mutex. Returns 0 on success.
 */
int shm_unlock(SharedState *state);

/*
 * Add a command to the history ring buffer (thread/process safe).
 * Automatically wraps around when HISTORY_CAPACITY is reached.
 */
void shm_history_add(SharedState *state, const char *cmd);

/*
 * Get the number of commands stored so far.
 */
int shm_history_count(SharedState *state);

/*
 * Retrieve a command by absolute index (0 = oldest still in buffer).
 * Returns NULL if index is out of range.
 */
const char *shm_history_get(SharedState *state, int index);

#endif /* IPC_SHM_SYNC_H */
