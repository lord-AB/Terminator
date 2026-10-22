/*
 * history_store.h — 10,000-command persistent history storage and retrieval
 */
#ifndef FEATURES_HISTORY_STORE_H
#define FEATURES_HISTORY_STORE_H

#include "common.h"
#include "ipc/shm_sync.h"

/*
 * Resolves the history file path (~/.terminator_history or $TERMINATOR_HISTORY_FILE).
 * Returns 0 on success, -1 on failure.
 */
int history_store_get_path(char *path_out, size_t max_len);

/*
 * Loads up to HISTORY_CAPACITY (10,000) commands from disk into shared memory.
 * If optional_path is NULL, defaults to history_store_get_path.
 * Returns the number of commands loaded.
 */
int history_store_load(SharedState *shm, const char *optional_path);

/*
 * Appends a command to shared memory and the history file on disk.
 * Automatically keeps file bounded to HISTORY_CAPACITY lines.
 * Returns 0 on success, -1 on error.
 */
int history_store_append(SharedState *shm, const char *cmd, const char *optional_path);

/*
 * Rewrites the history file on disk with the current entries in shared memory.
 * Returns 0 on success, -1 on error.
 */
int history_store_save(SharedState *shm, const char *optional_path);

#endif /* FEATURES_HISTORY_STORE_H */
