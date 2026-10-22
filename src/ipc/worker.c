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
#include "exec/shell_ctx.h"
#include "exec/builtins.h"
#include "exec/pipeline.h"
#include "exec/jobs.h"
#include "exec/redirect.h"
#include "parser/subst.h"
#include "parser/expand.h"
#include "parser/lexer.h"
#include "parser/ast.h"
#include "features/history_store.h"

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

/* ── Tab worker shell loop ─────────────────────────────────────────── */
void *worker_entry(void *arg) {
    WorkerArgs *wa = (WorkerArgs *)arg;
    int read_fd  = wa->read_fd;
    int write_fd = wa->write_fd;
    int tab_id   = wa->tab_id;
    SharedState *shared = wa->shared;

    TERM_LOG("tab[%d]: worker started (read_fd=%d, write_fd=%d)", tab_id, read_fd, write_fd);

    /* Initialize tab's local shell context on heap to prevent thread stack overflow */
    ShellContext *ctx = shell_ctx_create(tab_id);
    if (!ctx) {
        TERM_LOG("tab[%d]: failed to allocate shell context", tab_id);
        close(read_fd);
        close(write_fd);
        free(wa);
        return NULL;
    }
    ctx->in_fd = read_fd;
    ctx->out_fd = write_fd;

    /* Send READY and initial CWD to GUI */
    proto_send(write_fd, TAB_MSG_READY, NULL, 0);
    proto_send(write_fd, TAB_MSG_CWD, ctx->cwd, (uint32_t)strlen(ctx->cwd));

    /* Shell command loop */
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

        if (msg_type == GUI_MSG_SIGNAL) {
            /* Signal received while idle at prompt */
            ctx->last_status = 130;
            continue;
        }

        if (msg_type == GUI_MSG_LINE) {
            /* Reap and report any completed background jobs */
            jobs_check_status(ctx->jobs, write_fd);

            /* Skip empty lines */
            if (msg_len == 0 || buf[0] == '\0') {
                continue;
            }

            /* Record command in shared history and disk */
            history_store_append(shared, buf, NULL);

            /* 1. Command substitution $(...) and `...` */
            char subst_buf[MAX_CMD_LEN];
            if (subst_expand(ctx, buf, subst_buf, sizeof(subst_buf)) < 0) {
                const char *err = "terminator: substitution error\n";
                proto_send(write_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
                ctx->last_status = 1;
                continue;
            }

            /* 2. Variable expansion ($VAR, $?, ${VAR}) and line continuation (\\\n) */
            char expanded_buf[MAX_CMD_LEN];
            if (expand_variables(ctx, subst_buf, expanded_buf, sizeof(expanded_buf)) < 0) {
                const char *err = "terminator: expansion error\n";
                proto_send(write_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
                ctx->last_status = 1;
                continue;
            }

            /* 3. Quote-aware tokenization */
            char *tokens[MAX_ARGS];
            int num_tokens = lexer_tokenize(expanded_buf, tokens, MAX_ARGS);

            if (num_tokens == 0) {
                continue;
            }

            /* 4. Parse AST (redirections, pipeline segments, background flag) */
            Pipeline pl;
            if (ast_parse(tokens, num_tokens, &pl) < 0) {
                const char *err = "terminator: syntax error near unexpected token\n";
                proto_send(write_fd, TAB_MSG_OUTPUT, err, (uint32_t)strlen(err));
                ctx->last_status = 2;
                lexer_free_tokens(tokens, num_tokens);
                continue;
            }
            lexer_free_tokens(tokens, num_tokens);
            strncpy(pl.cmd_str, buf, sizeof(pl.cmd_str) - 1);
            pl.cmd_str[sizeof(pl.cmd_str) - 1] = '\0';

            if (pl.num_commands == 0) {
                pipeline_free(&pl);
                continue;
            }

            /* Execute command / pipeline */
            if (pl.num_commands == 1 && pl.commands[0].argc > 0 &&
                builtins_is_builtin(pl.commands[0].argv[0])) {
                int res = builtins_exec_cmd(ctx, &pl.commands[0], write_fd, shared);
                if (res == BUILTIN_EXIT) {
                    TERM_LOG("tab[%d]: exit built-in invoked, exiting", tab_id);
                    pipeline_free(&pl);
                    break;
                }
            } else {
                /* Execute pipeline (1 or more commands connected by pipes, with redirections) */
                pipeline_exec_pipeline(ctx, &pl, write_fd);
            }

            pipeline_free(&pl);
        }
    }

    /* Cleanup */
    shell_ctx_destroy(ctx);
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
