/*
 * worker.c — Tab worker spawner (Thread mode & Process mode)
 *
 * Creates two unidirectional pipes per tab and spawns the tab's shell
 * loop as either a pthread (Thread mode) or a fork()ed child (Process mode).
 *
 * Pipe layout (from GUI's perspective):
 *
 *   GUI ──write──→ gui_to_tab[1] ───→ gui_to_tab[0] ──read──→ TAB
 *   GUI ──read───← tab_to_gui[0] ←─── tab_to_gui[1] ──write── TAB
 */
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include "ipc/worker.h"
#include "ipc/protocol.h"

/* ── Initialize a TabWorker to a clean state ────────────────────────── */
void worker_init(TabWorker *w) {
    memset(w, 0, sizeof(TabWorker));
    w->tab_id       = -1;
    w->state        = TAB_UNUSED;
    w->gui_write_fd = -1;
    w->gui_read_fd  = -1;
    w->tab_read_fd  = -1;
    w->tab_write_fd = -1;
    w->child_pid    = -1;
    w->shared       = NULL;
}

/* ── Dummy tab worker loop (Stage 0 echo-back) ─────────────────────── */
void *worker_entry(void *arg) {
    WorkerArgs *wa = (WorkerArgs *)arg;
    int read_fd  = wa->read_fd;
    int write_fd = wa->write_fd;
    int tab_id   = wa->tab_id;
    SharedState *shared = wa->shared;

    TERM_LOG("tab[%d]: worker started (read_fd=%d, write_fd=%d)", tab_id, read_fd, write_fd);

    /* Send a READY message to GUI */
    proto_send(write_fd, TAB_MSG_READY, NULL, 0);

    /* Echo loop: read messages from GUI, echo them back */
    char buf[MAX_CMD_LEN];
    uint32_t msg_type, msg_len;

    while (1) {
        if (proto_recv(read_fd, &msg_type, buf, sizeof(buf) - 1, &msg_len) < 0) {
            TERM_LOG("tab[%d]: pipe closed or read error, exiting", tab_id);
            break;
        }

        buf[msg_len] = '\0';

        if (msg_type == GUI_MSG_CLOSE) {
            TERM_LOG("tab[%d]: received CLOSE message, exiting", tab_id);
            break;
        }

        if (msg_type == GUI_MSG_LINE) {
            TERM_LOG("tab[%d]: received line: \"%s\"", tab_id, buf);

            /* Add to shared history */
            shm_history_add(shared, buf);

            /* Echo back as output */
            char response[MAX_CMD_LEN + 64];
            int rlen = snprintf(response, sizeof(response),
                                "tab[%d] echo: %s", tab_id, buf);

            proto_send(write_fd, TAB_MSG_OUTPUT, response, (uint32_t)rlen);
        }
    }

    /* Cleanup */
    close(read_fd);
    close(write_fd);
    free(wa);

    TERM_LOG("tab[%d]: worker exited", tab_id);
    return NULL;
}

/* ── Spawn a tab worker ─────────────────────────────────────────────── */
int worker_spawn(TabWorker *w, int tab_id, ExecMode mode, SharedState *shared) {
    int gui_to_tab[2];  /* [0]=read(tab), [1]=write(gui) */
    int tab_to_gui[2];  /* [0]=read(gui), [1]=write(tab) */

    /* Create pipe: GUI → Tab */
    if (pipe(gui_to_tab) < 0) {
        TERM_LOG("worker_spawn: pipe(gui_to_tab) failed: %s", strerror(errno));
        return TERM_ERR_PIPE;
    }

    /* Create pipe: Tab → GUI */
    if (pipe(tab_to_gui) < 0) {
        TERM_LOG("worker_spawn: pipe(tab_to_gui) failed: %s", strerror(errno));
        close(gui_to_tab[0]);
        close(gui_to_tab[1]);
        return TERM_ERR_PIPE;
    }

    /* Fill in the worker struct */
    w->tab_id       = tab_id;
    w->mode         = mode;
    w->shared       = shared;
    w->gui_write_fd = gui_to_tab[1];   /* GUI writes here */
    w->gui_read_fd  = tab_to_gui[0];   /* GUI reads here  */
    w->tab_read_fd  = gui_to_tab[0];   /* Tab reads here  */
    w->tab_write_fd = tab_to_gui[1];   /* Tab writes here */

    /* Prepare worker arguments (heap-allocated, freed by worker) */
    WorkerArgs *wa = (WorkerArgs *)malloc(sizeof(WorkerArgs));
    if (!wa) {
        TERM_LOG("worker_spawn: malloc failed");
        close(gui_to_tab[0]); close(gui_to_tab[1]);
        close(tab_to_gui[0]); close(tab_to_gui[1]);
        return TERM_ERR_ALLOC;
    }
    wa->tab_id   = tab_id;
    wa->read_fd  = gui_to_tab[0];   /* tab reads from this end */
    wa->write_fd = tab_to_gui[1];   /* tab writes to this end  */
    wa->shared   = shared;
    wa->mode     = mode;

    if (mode == MODE_THREAD) {
        /* ── Thread mode: spawn a pthread ───────────────────────────── */
        int err = pthread_create(&w->thread_id, NULL, worker_entry, wa);
        if (err != 0) {
            TERM_LOG("worker_spawn: pthread_create failed: %s", strerror(err));
            free(wa);
            close(gui_to_tab[0]); close(gui_to_tab[1]);
            close(tab_to_gui[0]); close(tab_to_gui[1]);
            return TERM_ERR_FORK;
        }
        w->state = TAB_RUNNING;
        TERM_LOG("tab[%d]: spawned as thread (tid=%lu)", tab_id, (unsigned long)w->thread_id);

    } else {
        /* ── Process mode: spawn via fork() ─────────────────────────── */
        pid_t pid = fork();
        if (pid < 0) {
            TERM_LOG("worker_spawn: fork failed: %s", strerror(errno));
            free(wa);
            close(gui_to_tab[0]); close(gui_to_tab[1]);
            close(tab_to_gui[0]); close(tab_to_gui[1]);
            return TERM_ERR_FORK;
        }

        if (pid == 0) {
            /* ── Child process ──────────────────────────────────────── */
            /* Close the GUI's ends of the pipes */
            close(gui_to_tab[1]);   /* GUI's write end */
            close(tab_to_gui[0]);   /* GUI's read end  */

            /* Run the worker loop (never returns normally) */
            worker_entry(wa);
            _exit(0);
        }

        /* ── Parent process ─────────────────────────────────────────── */
        /* Close the tab's ends of the pipes */
        close(gui_to_tab[0]);   /* Tab's read end  */
        close(tab_to_gui[1]);   /* Tab's write end */
        w->tab_read_fd  = -1;   /* no longer valid in parent */
        w->tab_write_fd = -1;

        w->child_pid = pid;
        w->state = TAB_RUNNING;
        TERM_LOG("tab[%d]: spawned as process (pid=%d)", tab_id, pid);
    }

    return TERM_OK;
}

/* ── Stop a tab worker ──────────────────────────────────────────────── */
int worker_stop(TabWorker *w) {
    if (w->state != TAB_RUNNING)
        return 0;

    TERM_LOG("tab[%d]: stopping worker...", w->tab_id);

    /* Send close message */
    proto_send(w->gui_write_fd, GUI_MSG_CLOSE, NULL, 0);

    if (w->mode == MODE_THREAD) {
        /* Wait for thread to finish */
        pthread_join(w->thread_id, NULL);
    } else {
        /* Wait for child process */
        int status;
        if (w->child_pid > 0) {
            waitpid(w->child_pid, &status, 0);
        }
    }

    /* Close GUI's ends of the pipes */
    safe_close(&w->gui_write_fd);
    safe_close(&w->gui_read_fd);

    w->state = TAB_STOPPED;
    TERM_LOG("tab[%d]: worker stopped", w->tab_id);
    return 0;
}
