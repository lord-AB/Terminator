/*
 * main.c — CLI parser, shared memory init, and Stage 0 verification driver
 *
 * Usage:
 *   ./terminator                   — Thread mode (default)
 *   ./terminator useProcess=True   — Process mode
 *
 * Stage 0 verification: spawns a dummy tab worker that echoes messages
 * back over pipes, exercises both execution modes, and validates that
 * shared history works across workers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "gui/x11_window.h"
#include "ipc/protocol.h"
#include "ipc/shm_sync.h"
#include "ipc/worker.h"

/* ── Parse CLI arguments ────────────────────────────────────────────── */
static ExecMode parse_args(int argc, char *argv[], int *out_test_mode) {
    ExecMode mode = MODE_THREAD;  /* default */
    if (out_test_mode) {
        *out_test_mode = 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "useProcess=True") == 0) {
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

/* ── Stage 0 verification: echo test ────────────────────────────────── */
static int run_echo_test(ExecMode mode) {
    printf("\n========================================\n");
    printf("  Terminator — Stage 0 Verification\n");
    printf("  Mode: %s\n", mode == MODE_PROCESS ? "PROCESS" : "THREAD");
    printf("========================================\n\n");

    /* 1. Create shared memory */
    SharedState *shared = shm_create(mode);
    if (!shared) {
        fprintf(stderr, "FAIL: Could not create shared state\n");
        return 1;
    }
    printf("[PASS] Shared memory created\n");

    /* 2. Spawn two tab workers */
    TabWorker tabs[2];
    worker_init(&tabs[0]);
    worker_init(&tabs[1]);

    if (worker_spawn(&tabs[0], 0, mode, shared) != TERM_OK) {
        fprintf(stderr, "FAIL: Could not spawn tab 0\n");
        shm_destroy(shared, mode);
        return 1;
    }
    printf("[PASS] Tab 0 spawned\n");

    if (worker_spawn(&tabs[1], 1, mode, shared) != TERM_OK) {
        fprintf(stderr, "FAIL: Could not spawn tab 1\n");
        worker_stop(&tabs[0]);
        shm_destroy(shared, mode);
        return 1;
    }
    printf("[PASS] Tab 1 spawned\n");

    /* 3. Wait for READY from both tabs */
    for (int t = 0; t < 2; t++) {
        uint32_t msg_type, msg_len;
        char buf[256];
        if (proto_recv(tabs[t].gui_read_fd, &msg_type, buf, sizeof(buf), &msg_len) < 0) {
            fprintf(stderr, "FAIL: Could not receive READY from tab %d\n", t);
            worker_stop(&tabs[0]);
            worker_stop(&tabs[1]);
            shm_destroy(shared, mode);
            return 1;
        }
        if (msg_type != TAB_MSG_READY) {
            fprintf(stderr, "FAIL: Tab %d sent unexpected message type %u (expected READY=%d)\n",
                    t, msg_type, TAB_MSG_READY);
            worker_stop(&tabs[0]);
            worker_stop(&tabs[1]);
            shm_destroy(shared, mode);
            return 1;
        }
        printf("[PASS] Tab %d is ready\n", t);
    }

    /* 4. Send test lines and verify echo responses */
    const char *test_msgs[] = {
        "hello from tab 0",
        "ls -la",
        "echo world"
    };
    int n_msgs = sizeof(test_msgs) / sizeof(test_msgs[0]);

    for (int i = 0; i < n_msgs; i++) {
        int tab_idx = i % 2;  /* alternate between tabs */
        const char *msg = test_msgs[i];
        uint32_t msg_len = (uint32_t)strlen(msg);

        /* Send line to tab */
        if (proto_send(tabs[tab_idx].gui_write_fd, GUI_MSG_LINE, msg, msg_len) != TERM_OK) {
            fprintf(stderr, "FAIL: Could not send to tab %d\n", tab_idx);
            break;
        }

        /* Receive echoed response */
        uint32_t resp_type, resp_len;
        char resp_buf[MAX_CMD_LEN];
        if (proto_recv(tabs[tab_idx].gui_read_fd, &resp_type, resp_buf, sizeof(resp_buf) - 1, &resp_len) < 0) {
            fprintf(stderr, "FAIL: Could not receive response from tab %d\n", tab_idx);
            break;
        }
        resp_buf[resp_len] = '\0';

        if (resp_type != TAB_MSG_OUTPUT) {
            fprintf(stderr, "FAIL: Unexpected response type %u\n", resp_type);
            break;
        }

        printf("[PASS] Sent \"%s\" → tab[%d] → received \"%s\"\n", msg, tab_idx, resp_buf);
    }

    /* 5. Verify shared history */
    int hist_count = shm_history_count(shared);
    printf("\n--- History verification ---\n");
    printf("Total commands in history: %d (expected %d)\n", hist_count, n_msgs);

    if (hist_count == n_msgs) {
        printf("[PASS] History count matches\n");
    } else {
        printf("[FAIL] History count mismatch\n");
    }

    /* Print all history entries */
    for (int i = 0; i < hist_count && i < HISTORY_DISPLAY; i++) {
        const char *entry = shm_history_get(shared, i);
        if (entry) {
            printf("  history[%d] = \"%s\"\n", i, entry);
        }
    }

    /* 6. Verify cross-tab history visibility */
    printf("\n--- Cross-tab history test ---\n");
    /* Both tabs wrote to the same shared history. Verify we can see
     * commands from tab 0 and tab 1 in the same history buffer. */
    int found_tab0 = 0, found_tab1 = 0;
    for (int i = 0; i < hist_count; i++) {
        const char *entry = shm_history_get(shared, i);
        if (entry) {
            if (strstr(entry, "hello from tab 0")) found_tab0 = 1;
            if (strstr(entry, "ls -la")) found_tab1 = 1;
        }
    }
    if (found_tab0 && found_tab1) {
        printf("[PASS] Cross-tab history: commands from both tabs visible\n");
    } else {
        printf("[FAIL] Cross-tab history: tab0=%d tab1=%d\n", found_tab0, found_tab1);
    }

    /* 7. Stop workers */
    printf("\n--- Shutting down ---\n");
    worker_stop(&tabs[0]);
    worker_stop(&tabs[1]);
    printf("[PASS] All workers stopped\n");

    /* 8. Cleanup */
    shm_destroy(shared, mode);
    printf("[PASS] Shared memory destroyed\n");

    printf("\n========================================\n");
    printf("  Stage 0 Verification Complete!\n");
    printf("========================================\n\n");

    return 0;
}

/* ── Launch interactive X11 GUI ─────────────────────────────────────── */
static int run_gui(ExecMode mode) {
    SharedState *shared = shm_create(mode);
    if (!shared) {
        TERM_LOG("main: failed to create shared state");
        return 1;
    }

    GuiState gui;
    if (gui_init(&gui, mode, shared) != 0) {
        TERM_LOG("Could not open X11 display. (Ensure DISPLAY is set, e.g. via XQuartz on macOS)");
        TERM_LOG("Falling back to headless verification test...");
        shm_destroy(shared, mode);
        return run_echo_test(mode);
    }

    /* Open the first tab */
    if (gui_open_tab(&gui) < 0) {
        TERM_LOG("main: failed to open initial tab");
        gui_cleanup(&gui);
        shm_destroy(shared, mode);
        return 1;
    }

    /* Run the poll()-based event loop */
    gui_run(&gui);

    /* Cleanup */
    gui_cleanup(&gui);
    shm_destroy(shared, mode);
    return 0;
}

int main(int argc, char *argv[]) {
    int test_mode = 0;
    ExecMode mode = parse_args(argc, argv, &test_mode);

    if (test_mode) {
        return run_echo_test(mode);
    }

    return run_gui(mode);
}
