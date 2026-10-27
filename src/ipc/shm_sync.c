/*
 * shm_sync.c — Shared memory creation/destruction and history ring buffer
 *
 * In Process mode: allocates shared memory via mmap(MAP_SHARED|MAP_ANONYMOUS)
 * and initializes a PTHREAD_PROCESS_SHARED mutex for cross-process locking.
 *
 * In Thread mode: uses malloc (shared within the single process) with
 * a standard pthread mutex.
 */
#include <sys/mman.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include "ipc/shm_sync.h"

/* ── Create and initialize shared state ─────────────────────────────── */
SharedState *shm_create(ExecMode mode) {
    SharedState *state = NULL;

    if (mode == MODE_PROCESS) {
        /* Allocate anonymous shared memory visible to forked children */
        state = (SharedState *)mmap(
            NULL,
            sizeof(SharedState),
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANONYMOUS,
            -1,     /* no file descriptor */
            0       /* no offset */
        );
        if (state == MAP_FAILED) {
            TERM_LOG("shm_create: mmap failed: %s", strerror(errno));
            return NULL;
        }
    } else {
        /* Thread mode: plain heap allocation (shared within the process) */
        state = (SharedState *)calloc(1, sizeof(SharedState));
        if (!state) {
            TERM_LOG("shm_create: calloc failed: %s", strerror(errno));
            return NULL;
        }
    }

    /* Zero out the entire region */
    memset(state, 0, sizeof(SharedState));
    state->use_sem = 0;

#ifndef __APPLE__
    if (mode == MODE_PROCESS) {
        /* On Linux, initialize POSIX process-shared unnamed semaphore in shared memory */
        if (sem_init(&state->sem, 1 /* pshared */, 1 /* initial value */) == 0) {
            state->use_sem = 1;
            TERM_LOG("shm_create: initialized process-shared POSIX semaphore");
        }
    }
#endif

    if (!state->use_sem) {
        /* Initialize process-shared mutex (portable across Linux and macOS) */
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);

        if (mode == MODE_PROCESS) {
            /* PTHREAD_PROCESS_SHARED allows the mutex to work across
             * forked processes when stored in shared memory */
            if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
                TERM_LOG("shm_create: pthread_mutexattr_setpshared failed");
                shm_destroy(state, mode);
                pthread_mutexattr_destroy(&attr);
                return NULL;
            }
        }

        if (pthread_mutex_init(&state->lock, &attr) != 0) {
            TERM_LOG("shm_create: pthread_mutex_init failed");
            shm_destroy(state, mode);
            pthread_mutexattr_destroy(&attr);
            return NULL;
        }

        pthread_mutexattr_destroy(&attr);
    }

    state->count = 0;
    state->head  = 0;

    TERM_LOG("shm_create: shared state allocated (%s mode, %zu bytes, sync: %s)",
             mode == MODE_PROCESS ? "process" : "thread",
             sizeof(SharedState),
             state->use_sem ? "POSIX semaphore" : "pthread_mutex");
    return state;
}

/* ── Destroy shared state ───────────────────────────────────────────── */
void shm_destroy(SharedState *state, ExecMode mode) {
    if (!state)
        return;

#ifndef __APPLE__
    if (state->use_sem) {
        sem_destroy(&state->sem);
    } else
#endif
    {
        pthread_mutex_destroy(&state->lock);
    }

    if (mode == MODE_PROCESS) {
        munmap(state, sizeof(SharedState));
    } else {
        free(state);
    }
}

/* ── Lock ───────────────────────────────────────────────────────────── */
int shm_lock(SharedState *state) {
    if (!state) return -1;
#ifndef __APPLE__
    if (state->use_sem) {
        return sem_wait(&state->sem);
    }
#endif
    return pthread_mutex_lock(&state->lock);
}

/* ── Unlock ─────────────────────────────────────────────────────────── */
int shm_unlock(SharedState *state) {
    if (!state) return -1;
#ifndef __APPLE__
    if (state->use_sem) {
        return sem_post(&state->sem);
    }
#endif
    return pthread_mutex_unlock(&state->lock);
}

/* ── Add a command to history ───────────────────────────────────────── */
void shm_history_add(SharedState *state, const char *cmd) {
    if (!cmd || cmd[0] == '\0')
        return;

    shm_lock(state);

    /* Write to current head position */
    strncpy(state->entries[state->head].cmd, cmd, MAX_CMD_LEN - 1);
    state->entries[state->head].cmd[MAX_CMD_LEN - 1] = '\0';
    state->entries[state->head].valid = 1;

    /* Advance head (ring buffer wrap) */
    state->head = (state->head + 1) % HISTORY_CAPACITY;
    state->count++;

    shm_unlock(state);
}

/* ── Get total command count ────────────────────────────────────────── */
int shm_history_count(SharedState *state) {
    shm_lock(state);
    int c = state->count;
    shm_unlock(state);
    return c;
}

/* ── Get command by absolute index ──────────────────────────────────── */
const char *shm_history_get(SharedState *state, int index) {
    shm_lock(state);

    /* How many entries are actually stored */
    int stored = state->count < HISTORY_CAPACITY ? state->count : HISTORY_CAPACITY;

    if (index < 0 || index >= stored) {
        shm_unlock(state);
        return NULL;
    }

    /*
     * Map absolute index to ring buffer position:
     * If buffer hasn't wrapped: slot = index
     * If buffer has wrapped:    oldest is at head, so slot = (head + index) % CAP
     */
    int slot;
    if (state->count <= HISTORY_CAPACITY) {
        slot = index;
    } else {
        slot = (state->head + index) % HISTORY_CAPACITY;
    }

    const char *result = state->entries[slot].valid ? state->entries[slot].cmd : NULL;
    shm_unlock(state);
    return result;
}
