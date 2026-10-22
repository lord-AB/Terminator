/*
 * history_store.c — Persistent history storage and retrieval implementation
 */
#include "features/history_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int history_store_get_path(char *path_out, size_t max_len) {
    if (!path_out || max_len == 0) return -1;

    const char *custom = getenv("TERMINATOR_HISTORY_FILE");
    if (custom && custom[0] != '\0') {
        strncpy(path_out, custom, max_len - 1);
        path_out[max_len - 1] = '\0';
        return 0;
    }

    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        snprintf(path_out, max_len, "%s/.terminator_history", home);
        if (access(home, W_OK) != 0) {
            strncpy(path_out, ".terminator_history", max_len - 1);
            path_out[max_len - 1] = '\0';
        }
    } else {
        strncpy(path_out, ".terminator_history", max_len - 1);
        path_out[max_len - 1] = '\0';
    }
    return 0;
}

int history_store_load(SharedState *shm, const char *optional_path) {
    if (!shm) return -1;

    char file_path[MAX_PATH_LEN];
    if (optional_path && optional_path[0] != '\0') {
        strncpy(file_path, optional_path, sizeof(file_path) - 1);
        file_path[sizeof(file_path) - 1] = '\0';
    } else {
        if (history_store_get_path(file_path, sizeof(file_path)) < 0) {
            return -1;
        }
    }

    FILE *fp = fopen(file_path, "r");
    if (!fp) {
        return 0; /* File does not exist yet; not an error */
    }

    char line[MAX_CMD_LEN * 2];
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len > 0) {
            /* Unescape \n and \\ */
            char unesc[MAX_CMD_LEN];
            size_t u = 0;
            for (size_t i = 0; i < len && u < sizeof(unesc) - 1; i++) {
                if (line[i] == '\\' && i + 1 < len) {
                    if (line[i + 1] == 'n') {
                        unesc[u++] = '\n';
                        i++;
                    } else if (line[i + 1] == '\\') {
                        unesc[u++] = '\\';
                        i++;
                    } else {
                        unesc[u++] = line[i];
                    }
                } else {
                    unesc[u++] = line[i];
                }
            }
            unesc[u] = '\0';
            shm_history_add(shm, unesc);
            count++;
            if (count >= HISTORY_CAPACITY) {
                break;
            }
        }
    }

    fclose(fp);
    return count;
}

int history_store_save(SharedState *shm, const char *optional_path) {
    if (!shm) return -1;

    char file_path[MAX_PATH_LEN];
    if (optional_path && optional_path[0] != '\0') {
        strncpy(file_path, optional_path, sizeof(file_path) - 1);
        file_path[sizeof(file_path) - 1] = '\0';
    } else {
        if (history_store_get_path(file_path, sizeof(file_path)) < 0) {
            return -1;
        }
    }

    FILE *fp = fopen(file_path, "w");
    if (!fp) {
        fp = fopen(".terminator_history", "w");
        if (!fp) {
            return 0;
        }
    }

    int total = shm_history_count(shm);
    int stored = total < HISTORY_CAPACITY ? total : HISTORY_CAPACITY;

    for (int i = 0; i < stored; i++) {
        const char *cmd = shm_history_get(shm, i);
        if (cmd && cmd[0] != '\0') {
            for (const char *p = cmd; *p; p++) {
                if (*p == '\n') {
                    fputs("\\n", fp);
                } else if (*p == '\\') {
                    fputs("\\\\", fp);
                } else {
                    fputc(*p, fp);
                }
            }
            fputc('\n', fp);
        }
    }

    fclose(fp);
    return 0;
}

int history_store_append(SharedState *shm, const char *cmd, const char *optional_path) {
    if (!shm || !cmd || cmd[0] == '\0') return -1;

    /* Add to in-memory shared history */
    shm_history_add(shm, cmd);

    char file_path[MAX_PATH_LEN];
    if (optional_path && optional_path[0] != '\0') {
        strncpy(file_path, optional_path, sizeof(file_path) - 1);
        file_path[sizeof(file_path) - 1] = '\0';
    } else {
        if (history_store_get_path(file_path, sizeof(file_path)) < 0) {
            return -1;
        }
    }

    FILE *fp = fopen(file_path, "a");
    if (!fp) {
        fp = fopen(".terminator_history", "a");
        if (!fp) {
            return 0; /* Silently ignore filesystem error in restricted sandboxes */
        }
    }

    for (const char *p = cmd; *p; p++) {
        if (*p == '\n') {
            fputs("\\n", fp);
        } else if (*p == '\\') {
            fputs("\\\\", fp);
        } else {
            fputc(*p, fp);
        }
    }
    fputc('\n', fp);
    fclose(fp);

    /* If history exceeds capacity, prune disk file to last 10,000 entries */
    int total = shm_history_count(shm);
    if (total > HISTORY_CAPACITY && (total % 500 == 0)) {
        history_store_save(shm, optional_path);
    }

    return 0;
}
