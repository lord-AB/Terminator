/*
 * main.c — CLI parser, shared memory initialization, and GUI lifecycle
 *
 * Usage:
 *   ./terminator                   — Thread mode (default, pthread)
 *   ./terminator useProcess=True   — Process mode (fork)
 *   ./terminator --test            — Headless integration verification
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "common.h"
#include "gui/x11_window.h"
#include "ipc/shm_sync.h"
#include "features/history_store.h"
#include "verification.h"

/* ── Parse CLI arguments ────────────────────────────────────────────── */
static ExecMode parse_args(int argc, char *argv[], int *out_test_mode) {
    ExecMode mode = MODE_THREAD;  /* default */
    if (out_test_mode) {
        *out_test_mode = 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "useProcess=True") == 0) {
            mode = MODE_PROCESS;
        } else if (strcmp(argv[i], "--test") == 0) {
            if (out_test_mode) {
                *out_test_mode = 1;
            }
        }
    }

    if (mode == MODE_PROCESS) {
        TERM_LOG("Execution mode: PROCESS (fork)");
    } else {
        TERM_LOG("Execution mode: THREAD (pthread)");
    }

    return mode;
}

/* ── Launch interactive X11 GUI ─────────────────────────────────────── */
static int run_gui(ExecMode mode) {
    SharedState *shared = shm_create(mode);
    if (!shared) {
        TERM_LOG("main: failed to create shared state");
        return 1;
    }

    /* Load persistent history from ~/.terminator_history */
    history_store_load(shared, NULL);

    GuiState gui;
    if (gui_init(&gui, mode, shared) != 0) {
        TERM_LOG("Could not open X11 display. (Ensure DISPLAY is set, e.g. via XQuartz on macOS)");
        TERM_LOG("Falling back to headless verification test...");
        shm_destroy(shared, mode);
        return run_verification_test(mode);
    }

    /* Open the first tab */
    if (gui_open_tab(&gui) < 0) {
        TERM_LOG("main: failed to open initial tab");
        gui_cleanup(&gui);
        history_store_save(shared, NULL);
        shm_destroy(shared, mode);
        return 1;
    }

    /* Run the poll()-based event loop */
    gui_run(&gui);

    /* Cleanup */
    gui_cleanup(&gui);
    history_store_save(shared, NULL);
    shm_destroy(shared, mode);
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);
    int test_mode = 0;
    ExecMode mode = parse_args(argc, argv, &test_mode);

    if (test_mode) {
        return run_verification_test(mode);
    }

    return run_gui(mode);
}
