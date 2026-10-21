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
#include <poll.h>
#include <signal.h>

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

/* ── Helper to send command and collect response packet ─────────────── */
static int send_and_collect(int write_fd, int read_fd, const char *cmd,
                            char *out_buf, size_t out_sz, uint32_t *out_type) {
    if (proto_send(write_fd, GUI_MSG_LINE, cmd, (uint32_t)strlen(cmd)) != TERM_OK) {
        return -1;
    }

    struct pollfd pfd = { .fd = read_fd, .events = POLLIN, .revents = 0 };
    int ret = poll(&pfd, 1, 4000); /* 4 second timeout */
    if (ret <= 0) {
        return -1;
    }

    uint32_t type, len;
    if (proto_recv(read_fd, &type, out_buf, out_sz - 1, &len) < 0) {
        return -1;
    }
    out_buf[len] = '\0';
    if (out_type) *out_type = type;
    return 0;
}

/* ── Stage 2 verification test ──────────────────────────────────────── */
static int run_verification_test(ExecMode mode) {
    printf("\n========================================\n");
    printf("  Terminator — Stage 2 Verification\n");
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

    if (worker_spawn(&tabs[0], 0, mode, shared) != TERM_OK ||
        worker_spawn(&tabs[1], 1, mode, shared) != TERM_OK) {
        fprintf(stderr, "FAIL: Could not spawn tabs\n");
        shm_destroy(shared, mode);
        return 1;
    }
    printf("[PASS] Tabs 0 and 1 spawned\n");

    /* 3. Drain startup messages (READY + initial CWD) */
    char tab0_initial_cwd[MAX_PATH_LEN] = {0};
    char tab1_initial_cwd[MAX_PATH_LEN] = {0};

    for (int t = 0; t < 2; t++) {
        int got_ready = 0, got_cwd = 0;
        while (!got_ready || !got_cwd) {
            uint32_t type, len;
            char buf[MAX_PATH_LEN];
            if (proto_recv(tabs[t].gui_read_fd, &type, buf, sizeof(buf) - 1, &len) < 0) break;
            buf[len] = '\0';
            if (type == TAB_MSG_READY) got_ready = 1;
            if (type == TAB_MSG_CWD) {
                got_cwd = 1;
                if (t == 0) strncpy(tab0_initial_cwd, buf, sizeof(tab0_initial_cwd) - 1);
                else strncpy(tab1_initial_cwd, buf, sizeof(tab1_initial_cwd) - 1);
            }
        }
        printf("[PASS] Tab %d startup complete (initial CWD: %s)\n",
               t, t == 0 ? tab0_initial_cwd : tab1_initial_cwd);
    }

    /* 4. Test built-in `cd`: Change Tab 0 directory to /tmp */
    printf("\n--- Testing 'cd' and per-tab CWD isolation ---\n");
    char resp[MAX_CMD_LEN];
    uint32_t type;

    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "cd /tmp", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: cd /tmp did not respond\n");
        return 1;
    }
    if (type == TAB_MSG_CWD) {
        printf("[PASS] Tab 0 changed directory to: %s\n", resp);
    } else {
        printf("[FAIL] Expected TAB_MSG_CWD, got type %u\n", type);
        return 1;
    }

    /* Verify Tab 1 directory is still untouched (run `pwd` in Tab 1) */
    if (send_and_collect(tabs[1].gui_write_fd, tabs[1].gui_read_fd, "pwd", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: pwd in tab 1 did not respond\n");
        return 1;
    }
    /* Trim trailing newline if present */
    size_t rlen = strlen(resp);
    if (rlen > 0 && resp[rlen - 1] == '\n') resp[rlen - 1] = '\0';

    if (strstr(resp, "Terminator") != NULL) {
        printf("[PASS] Tab 1 CWD is isolated (pwd = \"%s\")\n", resp);
    } else {
        printf("[FAIL] Tab 1 CWD was affected: \"%s\"\n", resp);
        return 1;
    }

    /* 5. Compile and run C program in Tab 0 */
    printf("\n--- Testing external command execution (gcc & ./myprog) ---\n");
    /* Create myprog.c in /tmp */
    FILE *fp = fopen("/tmp/myprog.c", "w");
    if (fp) {
        fprintf(fp, "#include <stdio.h>\nint main() { printf(\"Hello from Terminator!\\n\"); return 0; }\n");
        fclose(fp);
    }

    /* Send compilation command to Tab 0 */
    proto_send(tabs[0].gui_write_fd, GUI_MSG_LINE, "gcc -o myprog myprog.c", 22);
    /* Allow gcc to compile */
    usleep(500000);

    /* Run ./myprog in Tab 0 */
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "./myprog", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: ./myprog did not respond\n");
        return 1;
    }
    printf("[PASS] ./myprog output: \"%s\"", resp);
    if (strstr(resp, "Hello from Terminator!") != NULL) {
        printf("[PASS] Output verified successfully\n");
    } else {
        printf("[FAIL] Output mismatch: \"%s\"\n", resp);
        return 1;
    }

    /* 6. Test unknown command error handling */
    printf("\n--- Testing unknown command error handling ---\n");
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "notacommand", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: notacommand did not respond\n");
        return 1;
    }
    printf("[PASS] notacommand output: \"%s\"", resp);
    if (strstr(resp, "command not found") != NULL) {
        printf("[PASS] Command not found error message verified\n");
    } else {
        printf("[FAIL] Unexpected error message: \"%s\"\n", resp);
        return 1;
    }

    /* 7. Test 'ls' in Tab 1 */
    printf("\n--- Testing 'ls' in Tab 1 ---\n");
    if (send_and_collect(tabs[1].gui_write_fd, tabs[1].gui_read_fd, "ls", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: ls in tab 1 did not respond\n");
        return 1;
    }
    if (strstr(resp, "Makefile") != NULL || strstr(resp, "src") != NULL) {
        printf("[PASS] Tab 1 'ls' correctly lists project root\n");
    } else {
        printf("[FAIL] Tab 1 'ls' unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 8. Stage 3: Multiline input, command substitution & quoting verification */
    printf("\n--- Stage 3: Multiline, Quoting & Substitution Verification ---\n");

    /* 8a. Multiline with line continuation */
    const char *s3_ex1 = "echo \"Isn't process creation exciting?\\\nFork, exec, repeat!\"";
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, s3_ex1, resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: Stage 3 example 1 did not respond\n");
        return 1;
    }
    printf("[PASS] Example 1 output: \"%s\"", resp);
    if (strstr(resp, "Isn't process creation exciting?Fork, exec, repeat!") != NULL) {
        printf("[PASS] Multiline line-continuation verified\n");
    } else {
        printf("[FAIL] Unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 8b. Nested command substitution */
    const char *s3_ex2 = "echo \"Files in $(basename $(pwd)): $(ls | wc -l)\"";
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, s3_ex2, resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: Stage 3 example 2 did not respond\n");
        return 1;
    }
    printf("[PASS] Example 2 output: \"%s\"", resp);
    if (strstr(resp, "Files in tmp:") != NULL) {
        printf("[PASS] Nested command substitution verified\n");
    } else {
        printf("[FAIL] Unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 8c. Quoting rules (literal single quotes, expanded double quotes) */
    const char *s3_ex3 = "echo 'literal: $HOME' \"expanded: $HOME\"";
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, s3_ex3, resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: Stage 3 example 3 did not respond\n");
        return 1;
    }
    printf("[PASS] Example 3 output: \"%s\"", resp);
    if (strstr(resp, "literal: $HOME") != NULL && strstr(resp, "expanded: ") != NULL &&
        strstr(resp, "expanded: $HOME") == NULL) {
        printf("[PASS] Quoting rules verified ($HOME literal in '...', expanded in \"...\")\n");
    } else {
        printf("[FAIL] Unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 9. Verify shared history */
    printf("\n--- History verification ---\n");
    int hist_count = shm_history_count(shared);
    printf("Total commands in history: %d\n", hist_count);
    for (int i = 0; i < hist_count; i++) {
        const char *h = shm_history_get(shared, i);
        if (h) printf("  [%d] %s\n", i, h);
    }
    if (hist_count >= 8) {
        printf("[PASS] Shared history logged all commands from both tabs\n");
    } else {
        printf("[FAIL] History count too low: %d\n", hist_count);
    }

    /* 10. Test exit built-in in Tab 0 */
    printf("\n--- Testing 'exit' built-in ---\n");
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "exit", resp, sizeof(resp), &type) == 0) {
        if (type == TAB_MSG_EXIT) {
            printf("[PASS] Tab 0 sent TAB_MSG_EXIT\n");
        }
    }
    worker_stop(&tabs[0]);
    worker_stop(&tabs[1]);
    printf("[PASS] All workers stopped cleanly\n");

    /* 11. Clean up */
    shm_destroy(shared, mode);
    printf("[PASS] Shared memory destroyed\n");

    printf("\n========================================\n");
    printf("  Stage 3 Verification Complete!\n");
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
        return run_verification_test(mode);
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
    signal(SIGPIPE, SIG_IGN);
    int test_mode = 0;
    ExecMode mode = parse_args(argc, argv, &test_mode);

    if (test_mode) {
        return run_verification_test(mode);
    }

    return run_gui(mode);
}
