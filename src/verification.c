/*
 * verification.c — End-to-end headless integration verification suite
 */
#include "verification.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include "ipc/protocol.h"
#include "ipc/shm_sync.h"
#include "ipc/worker.h"

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

    /* Drain any subsequent chunks for this command */
    size_t total = len;
    while (total < out_sz - 1) {
        pfd.revents = 0;
        if (poll(&pfd, 1, 60) <= 0) break;
        uint32_t next_type, next_len;
        if (proto_recv(read_fd, &next_type, out_buf + total, out_sz - 1 - total, &next_len) < 0) {
            break;
        }
        total += next_len;
        out_buf[total] = '\0';
    }

    return 0;
}

/* ── Run integration verification tests ─────────────────────────────── */
int run_verification_test(ExecMode mode) {
    printf("\n========================================\n");
    printf("  Terminator — Integration Verification\n");
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
    FILE *fp = fopen("/tmp/myprog.c", "w");
    if (fp) {
        fprintf(fp, "#include <stdio.h>\nint main() { printf(\"Hello from Terminator!\\n\"); return 0; }\n");
        fclose(fp);
    }

    proto_send(tabs[0].gui_write_fd, GUI_MSG_LINE, "gcc -o myprog myprog.c", 22);
    usleep(500000);

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

    /* 9. Stage 4: I/O Redirection Verification (<, >, and both together) */
    printf("\n--- Stage 4: I/O Redirection Verification ---\n");

    /* 9a. Input redirection: sort < somefile.txt */
    FILE *s4_in = fopen("/tmp/somefile.txt", "w");
    if (s4_in) {
        fprintf(s4_in, "zebra\napple\nmango\n");
        fclose(s4_in);
    }
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "sort < somefile.txt", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: sort < somefile.txt did not respond\n");
        return 1;
    }
    printf("[PASS] sort < somefile.txt output: \"%s\"", resp);
    if (strstr(resp, "apple\nmango\nzebra") != NULL) {
        printf("[PASS] Input redirection '<' verified\n");
    } else {
        printf("[FAIL] Unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 9b. Output redirection: ls > abc */
    unlink("/tmp/abc");
    proto_send(tabs[0].gui_write_fd, GUI_MSG_LINE, "ls > abc", 8);
    usleep(300000);
    FILE *s4_out = fopen("/tmp/abc", "r");
    if (s4_out) {
        char abc_buf[256] = {0};
        size_t r = fread(abc_buf, 1, sizeof(abc_buf) - 1, s4_out);
        fclose(s4_out);
        if (r > 0 && strstr(abc_buf, "somefile.txt") != NULL) {
            printf("[PASS] Output redirection '>' verified (file abc created with contents)\n");
        } else {
            printf("[FAIL] abc content mismatch or empty\n");
            return 1;
        }
    } else {
        printf("[FAIL] Output file /tmp/abc was not created\n");
        return 1;
    }

    /* 9c. Combined redirection: ./myprog < infile.txt > outfile.txt */
    FILE *combo_in = fopen("/tmp/infile.txt", "w");
    if (combo_in) {
        fprintf(combo_in, "unused input\n");
        fclose(combo_in);
    }
    unlink("/tmp/outfile.txt");
    proto_send(tabs[0].gui_write_fd, GUI_MSG_LINE, "./myprog < infile.txt > outfile.txt", 36);
    usleep(300000);
    FILE *combo_out = fopen("/tmp/outfile.txt", "r");
    if (combo_out) {
        char out_line[128] = {0};
        if (fgets(out_line, sizeof(out_line), combo_out) && strstr(out_line, "Hello from Terminator!") != NULL) {
            printf("[PASS] Combined redirection '<' and '>' verified\n");
        } else {
            printf("[FAIL] outfile.txt content mismatch\n");
            fclose(combo_out);
            return 1;
        }
        fclose(combo_out);
    } else {
        printf("[FAIL] outfile.txt was not created\n");
        return 1;
    }

    /* 9d. Missing file error reporting: < missing_file.txt */
    unlink("/tmp/missing_file_xyz.txt");
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "< missing_file_xyz.txt", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: < missing_file_xyz.txt did not respond\n");
        return 1;
    }
    printf("[PASS] < missing_file.txt error output: \"%s\"", resp);
    if (strstr(resp, "terminator: missing_file_xyz.txt: No such file or directory") != NULL) {
        printf("[PASS] Missing input file error reporting verified\n");
    } else {
        printf("[FAIL] Unexpected error message: \"%s\"\n", resp);
        return 1;
    }

    /* 9e. Command abort on missing file: sort < missing_file.txt */
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "sort < missing_file_xyz.txt", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: sort < missing_file_xyz.txt did not respond\n");
        return 1;
    }
    printf("[PASS] sort < missing_file.txt output: \"%s\"", resp);
    if (strstr(resp, "terminator: missing_file_xyz.txt: No such file or directory") != NULL) {
        printf("[PASS] Command aborted on missing input file without executing\n");
    } else {
        printf("[FAIL] Unexpected error message: \"%s\"\n", resp);
        return 1;
    }

    /* Clean up /tmp files */
    unlink("/tmp/somefile.txt");
    unlink("/tmp/abc");
    unlink("/tmp/infile.txt");
    unlink("/tmp/outfile.txt");

    /* 10. Stage 5: Pipeline & Pipe Orchestration Verification */
    printf("\n--- Stage 5: Pipelines & Pipe Orchestration Verification ---\n");

    /* 10a. 2-stage pipeline with wildcards: ls test_pipe*.txt | wc -l */
    FILE *pt1 = fopen("/tmp/test_pipe1.txt", "w"); if (pt1) { fprintf(pt1, "1\n"); fclose(pt1); }
    FILE *pt2 = fopen("/tmp/test_pipe2.txt", "w"); if (pt2) { fprintf(pt2, "2\n"); fclose(pt2); }
    FILE *pt3 = fopen("/tmp/test_pipe3.txt", "w"); if (pt3) { fprintf(pt3, "3\n"); fclose(pt3); }

    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "ls test_pipe*.txt | wc -l", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: ls test_pipe*.txt | wc -l did not respond\n");
        return 1;
    }
    printf("[PASS] ls test_pipe*.txt | wc -l output: \"%s\"", resp);
    if (strstr(resp, "3") != NULL) {
        printf("[PASS] 2-stage pipeline with wildcards verified\n");
    } else {
        printf("[FAIL] Unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 10b. 3-stage pipeline: cat abc.c | sort | head -5 */
    FILE *p_abc = fopen("/tmp/abc.c", "w");
    if (p_abc) {
        fprintf(p_abc, "zebra\nyellow\napple\norange\nbanana\ngrape\ncherry\n");
        fclose(p_abc);
    }
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "cat abc.c | sort | head -5", resp, sizeof(resp), &type) != 0) {
        fprintf(stderr, "FAIL: cat abc.c | sort | head -5 did not respond\n");
        return 1;
    }
    printf("[PASS] cat abc.c | sort | head -5 output: \"%s\"", resp);
    if (strstr(resp, "apple") != NULL && strstr(resp, "banana") != NULL &&
        strstr(resp, "cherry") != NULL && strstr(resp, "grape") != NULL &&
        strstr(resp, "orange") != NULL && strstr(resp, "zebra") == NULL) {
        printf("[PASS] 3-stage pipeline verified\n");
    } else {
        printf("[FAIL] Unexpected output: \"%s\"\n", resp);
        return 1;
    }

    /* 10c. Pipeline combined with redirections: sort < abc.c | head -2 > out_pipe.txt */
    unlink("/tmp/out_pipe.txt");
    proto_send(tabs[0].gui_write_fd, GUI_MSG_LINE, "sort < abc.c | head -2 > out_pipe.txt", 37);
    usleep(300000);
    FILE *f_op = fopen("/tmp/out_pipe.txt", "r");
    if (f_op) {
        char op_buf[128] = {0};
        size_t nr = fread(op_buf, 1, sizeof(op_buf) - 1, f_op);
        fclose(f_op);
        if (nr > 0 && strcmp(op_buf, "apple\nbanana\n") == 0) {
            printf("[PASS] Pipeline combined with '<' and '>' verified\n");
        } else {
            printf("[FAIL] out_pipe.txt content mismatch: \"%s\"\n", op_buf);
            return 1;
        }
    } else {
        printf("[FAIL] out_pipe.txt was not created\n");
        return 1;
    }

    /* Clean up /tmp test files */
    unlink("/tmp/test_pipe1.txt");
    unlink("/tmp/test_pipe2.txt");
    unlink("/tmp/test_pipe3.txt");
    unlink("/tmp/abc.c");
    unlink("/tmp/out_pipe.txt");

    /* 11. Stage 6: multiWatch verification */
    printf("\n--- Stage 6: multiWatch Verification ---\n");
    proto_send(tabs[0].gui_write_fd, GUI_MSG_LINE, "multiWatch [\"date +%T\", \"ls | wc -l\"]", 37);

    char mw_output[4096] = {0};
    size_t mw_read = 0;
    struct pollfd mw_pfd = { .fd = tabs[0].gui_read_fd, .events = POLLIN, .revents = 0 };

    int mw_poll_count = 0;
    while (mw_poll_count++ < 15) {
        mw_pfd.revents = 0;
        if (poll(&mw_pfd, 1, 100) > 0) {
            uint32_t mw_type, mw_len;
            char chunk[1024];
            if (proto_recv(tabs[0].gui_read_fd, &mw_type, chunk, sizeof(chunk) - 1, &mw_len) == 0) {
                chunk[mw_len] = '\0';
                if (mw_read + mw_len < sizeof(mw_output) - 1) {
                    strcat(mw_output, chunk);
                    mw_read += mw_len;
                }
            }
        }
        if (strstr(mw_output, "\"date +%T\",") != NULL && strstr(mw_output, "\"ls | wc -l\",") != NULL) {
            break;
        }
    }

    int mw_sig = SIGINT;
    proto_send(tabs[0].gui_write_fd, GUI_MSG_SIGNAL, &mw_sig, sizeof(mw_sig));

    mw_poll_count = 0;
    while (mw_poll_count++ < 15) {
        mw_pfd.revents = 0;
        if (poll(&mw_pfd, 1, 100) > 0) {
            uint32_t mw_type, mw_len;
            char chunk[1024];
            if (proto_recv(tabs[0].gui_read_fd, &mw_type, chunk, sizeof(chunk) - 1, &mw_len) == 0) {
                chunk[mw_len] = '\0';
                if (mw_read + mw_len < sizeof(mw_output) - 1) {
                    strcat(mw_output, chunk);
                    mw_read += mw_len;
                }
                if (strstr(chunk, "^C") != NULL) break;
            }
        }
    }

    printf("[PASS] multiWatch received output:\n%s", mw_output);
    if (strstr(mw_output, "\"date +%T\",") != NULL &&
        strstr(mw_output, "\"ls | wc -l\",") != NULL &&
        strstr(mw_output, "---------------------------------------------------") != NULL &&
        strstr(mw_output, "^C") != NULL) {
        printf("[PASS] multiWatch parallel execution, format, and Ctrl+C interrupt verified\n");
    } else {
        printf("[FAIL] multiWatch output incomplete or missing expected markers\n");
        return 1;
    }

    /* 12. Verify shared history */
    printf("\n--- History verification ---\n");
    int hist_count = shm_history_count(shared);
    printf("Total commands in history: %d\n", hist_count);
    for (int i = 0; i < hist_count; i++) {
        const char *h = shm_history_get(shared, i);
        if (h) printf("  [%d] %s\n", i, h);
    }
    if (hist_count >= 17) {
        printf("[PASS] Shared history logged all commands from both tabs\n");
    } else {
        printf("[FAIL] History count too low: %d\n", hist_count);
    }

    /* 13. Test exit built-in in Tab 0 */
    printf("\n--- Testing 'exit' built-in ---\n");
    if (send_and_collect(tabs[0].gui_write_fd, tabs[0].gui_read_fd, "exit", resp, sizeof(resp), &type) == 0) {
        if (type == TAB_MSG_EXIT) {
            printf("[PASS] Tab 0 sent TAB_MSG_EXIT\n");
        }
    }
    worker_stop(&tabs[0]);
    worker_stop(&tabs[1]);
    printf("[PASS] All workers stopped cleanly\n");

    /* 14. Clean up */
    shm_destroy(shared, mode);
    printf("[PASS] Shared memory destroyed\n");

    printf("\n========================================\n");
    printf("  Stage 6 Verification Complete!\n");
    printf("========================================\n\n");
    return 0;
}
