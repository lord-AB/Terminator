# Terminator: System Design & Architecture Document

**Computing Laboratory (CS69201) · Term Project**  
*Comprehensive Technical Architecture, IPC Specification, and Implementation Guide*

---

## Table of Contents
1. [X11 GUI & Non-Blocking `poll()` Event Loop](#1-x11-gui--non-blocking-poll-event-loop)
2. [Concurrency Architecture: Thread Mode vs. Process Mode](#2-concurrency-architecture-thread-mode-vs-process-mode)
3. [Process & Job Management](#3-process--job-management)
4. [IPC Architecture & Pipeline Redirection](#4-ipc-architecture--pipeline-redirection)
5. [Data Structures & Algorithms](#5-data-structures--algorithms)
   - [5.1 Persistent Shared History & LCS Search](#51-persistent-shared-history--lcs-search)
   - [5.2 Auto-Completion & Longest Common Prefix (LCP)](#52-auto-completion--longest-common-prefix-lcp)
6. [Signal Handling & Event Interception](#6-signal-handling--event-interception)
7. [Bash Script Interpreter & Syntax Validator](#7-bash-script-interpreter--syntax-validator)
8. [Hotkey Customization & Keybinding System](#8-hotkey-customization--keybinding-system)

---

## 1. X11 GUI & Non-Blocking `poll()` Event Loop

Terminator implements a custom, hardware-accelerated X11 window using low-level `libX11` primitives without external terminal toolkits:

```
+-------------------------------------------------------------------+
|  [ 1: ~/project $ ] [ 2: /tmp ] [ + ]             (Tab Bar Area)  |
+-------------------------------------------------------------------+
|  user@terminator> ls -la                                          |
|  total 560                                                        |
|  -rw-r--r--  1 user  staff   6878 Oct 22 17:28 Makefile           |
|  -rw-r--r--  1 user  staff  11470 Oct 21 20:49 design.md          |
|                                                                   |
|  user@terminator> ./myprog def.txt abc_                           |
|  +---------------------------------------+                        |
|  | 1. abc.txt   2. abcd.txt              |  (Dynamic Suggestions) |
|  +---------------------------------------+                        |
+-------------------------------------------------------------------+
```

### 1.1 Window Lifecycle & UX Controls
* **X11 Setup:** `gui_init()` connects via `XOpenDisplay` and creates an unbacked top-level window selecting `ExposureMask`, `KeyPressMask`, `ButtonPressMask`, and `StructureNotifyMask`.
* **Double Buffering:** Draws exclusively into an off-screen `Pixmap` backbuffer copied atomically to the window via `XCopyArea` followed by `XFlush`, eliminating visual flicker.
* **Monospace Grid:** Character metrics derived from `XLoadQueryFont`:
  $$\text{cols} = \frac{\text{win\_width} - 8}{\text{char\_width}}, \quad \text{rows} = \frac{\text{win\_height} - \text{bar\_height}}{\text{char\_height}}$$
* **Tab Controls:**
  * *Open Tab:* Left-click `[ + ]` or press `Ctrl+T` / `Cmd+T`.
  * *Switch Tab:* Left-click a tab header or press `Alt+1`..`9` / `Cmd+1`..`9`.
  * *Close Tab:* Middle-click / Right-click a tab header, type `exit`, or press `Ctrl+D` on an empty line.
* **Scrollback:** Mouse wheel (Button 4 up, Button 5 down) or `Shift+Up` / `Shift+Down` scrolls the buffer. New commands or Enter automatically snap back to the bottom prompt.

### 1.2 Unified Non-Blocking `poll()` Loop
A single `poll()` syscall multiplexes X server events and tab worker pipe descriptors:
```c
struct pollfd pfds[MAX_TABS + 1];
pfds[0].fd = ConnectionNumber(gui->display); pfds[0].events = POLLIN;
for (int i = 0; i < MAX_TABS; i++) {
    if (gui->tabs[i].in_use) {
        pfds[nfds].fd = gui->tabs[i].worker.gui_read_fd;
        pfds[nfds++].events = POLLIN;
    }
}
poll(pfds, nfds, 10); /* 10ms tick for responsive UI */
```
* **X11 Traffic (`pfds[0]`):** Pending events are non-blockingly drained using `XPending(dpy)` and dispatched to keypress, mouse click, or resize handlers.
* **Tab Pipe Traffic (`pfds[tab]`):** Data is read via `proto_recv()`, appended to the tab's `ScreenBuf`, and redrawn.

---

## 2. Concurrency Architecture: Thread Mode vs. Process Mode

```
                            +-----------------------+
                            |   Terminator GUI      |
                            |   (Main Process)      |
                            +-----------+-----------+
                                        |
               +------------------------+------------------------+
               | (Thread Mode)                                   | (Process Mode)
               v                                                 v
    +--------------------+                            +--------------------+
    | Tab 0: pthread_t   |                            | Tab 0: fork() PID  |
    | Tab 1: pthread_t   |                            | Tab 1: fork() PID  |
    +---------+----------+                            +---------+----------+
              |                                                 |
              +-------------------+   +-------------------------+
                                  v   v
                      +-----------------------------+
                      |   Anonymous Shared Memory   |
                      |   - 10,000 Command History  |
                      |   - Pshared Mutex / Semaphore|
                      +-----------------------------+
```

### 2.1 CLI Argument Parsing
* **Thread Mode (Default):** `./terminator`
* **Process Mode:** `./terminator useProcess=True` (also accepts `./terminator use Process True` or `./terminator useProcess`, case-insensitive).

### 2.2 Execution Modes & CWD Isolation
* **Thread Mode:** Each tab runs as a POSIX thread (`pthread_create`). To prevent process-wide `chdir()` conflicts, each tab maintains an isolated virtual working directory (`ctx->cwd`) updated via `realpath()`. For command execution, forked child processes invoke `chdir(ctx->cwd)` immediately before `execvp()`.
* **Process Mode:** Each tab runs as an independent forked process, naturally isolating kernel-level working directories.

### 2.3 Shared Memory & Cross-Process Synchronization
All tabs share a 10,000-command history buffer via `SharedState` (~41 MB):
* **Allocation:** In Process Mode, allocated via anonymous memory `mmap(MAP_SHARED | MAP_ANONYMOUS)`, avoiding dangling files in `/dev/shm`. In Thread Mode, allocated via heap `calloc()`.
* **Synchronization:** Uses an unnamed POSIX semaphore (`sem_t`, `pshared = 1`) on Linux, and a process-shared mutex (`PTHREAD_PROCESS_SHARED`) on macOS / fallback. `shm_lock()` and `shm_unlock()` wrap locking transparently.

---

## 3. Process & Job Management

Every pipeline or command is isolated into its own process group (`setpgid`).

```
       Tab Worker (Parent)
               | fork()
               v
       +---------------------------------------------+
       | Process Group (PGID = PID_1)                |
       |  [Child 1: cat]  ==pipe==>  [Child 2: wc]   |
       +---------------------------------------------+
```

### 3.1 Foreground Multiplexing & Signal Delivery
* **PGID Tracking:** The first pipeline child calls `setpgid(0, 0)` (`pgid = pids[0]`); subsequent children and the parent worker call `setpgid(pid, pgid)` to prevent race conditions.
* **Non-Blocking Wait:** While foreground jobs run, the tab worker multiplexes output capture and GUI signal pipes using `poll()`.
* **Signals:** `Ctrl+C` sends `kill(-pgid, SIGINT)`; `Ctrl+Z` sends `kill(-pgid, SIGTSTP)`. Suspended jobs are recorded in the Job Table as `STOPPED` (`[1]+ Stopped <cmd>`).

### 3.2 Background Execution & Built-ins
* **Background (`cmd &`):** Spawns without waiting, prints `[job_id] <pgid>`, and immediately returns to prompt.
* **Asynchronous Reaping:** `jobs_check_status()` periodically checks background jobs via `waitpid(-1, &status, WNOHANG)` and prints completion notifications (`[1]+ Done <cmd>`).
* **Job Control Built-ins:** `jobs` lists tracked jobs; `bg %n` resumes a suspended job in the background via `SIGCONT`; `fg %n` brings a job to foreground and resumes monitoring.

---

## 4. IPC Architecture & Pipeline Redirection

### 4.1 Framing Protocol
Communication between GUI and tabs uses two unidirectional pipe pairs with fixed 8-byte framing (`type` + `length`):

| Message Type | Direction | Content / Purpose |
| :--- | :--- | :--- |
| `TAB_MSG_READY` | Tab $\to$ GUI | Worker startup complete |
| `TAB_MSG_OUTPUT` | Tab $\to$ GUI | Stdout/stderr chunk to append |
| `TAB_MSG_PROMPT` | Tab $\to$ GUI | Updated prompt string |
| `TAB_MSG_CWD` | Tab $\to$ GUI | Tab CWD change (updates header) |
| `TAB_MSG_CLEAR` | Tab $\to$ GUI | Clear screen request (`Ctrl+L`) |
| `TAB_MSG_EXIT` | Tab $\to$ GUI | Shell exit notification with status |
| `GUI_MSG_LINE` | GUI $\to$ Tab | Committed command line |
| `GUI_MSG_KEYPRESS` | GUI $\to$ Tab | Single keystroke character |
| `GUI_MSG_PASTE` | GUI $\to$ Tab | Pasted clipboard text block |
| `GUI_MSG_SIGNAL` | GUI $\to$ Tab | Forwarded signal (`SIGINT`, `SIGTSTP`) |
| `GUI_MSG_CLOSE` | GUI $\to$ Tab | Tab close request |
| `GUI_MSG_RESIZE` | GUI $\to$ Tab | Terminal dimension update |

`proto_send()` and `proto_recv()` guarantee full packet framing without partial read tearing. `proto_send_text()` automatically writes directly to `stdout`/`stderr` or frames into `TAB_MSG_OUTPUT` over pipes.

### 4.2 Pipeline Orchestration & Redirection
For an $N$-stage pipeline:
1. $N-1$ inter-command pipes and 1 final capture pipe are created.
2. For child $i \in [0, N-1]$: stdin binds to `pipes[i-1][0]` ($i > 0$); stdout binds to `pipes[i][1]` ($i < N-1$) or capture pipe ($i = N-1$).
3. **Explicit Redirection Overrides:** `<` opens `O_RDONLY` and binds to `STDIN_FILENO` (aborts execution if missing); `>` / `>>` opens `O_WRONLY | O_CREAT | (O_TRUNC | O_APPEND)` and binds to `STDOUT_FILENO`.
4. All unused descriptors are closed in child and parent so EOF cleanly propagates.

---

## 5. Data Structures & Algorithms

### 5.1 Persistent Shared History & LCS Search
The shared memory segment maintains a rolling ring buffer storing up to 10,000 commands:
```c
typedef struct {
    char cmd[MAX_CMD_LEN];      /* 4096-byte command string */
    int  valid;                 /* 1 if slot holds a valid entry */
} HistoryEntry;

typedef struct SharedState {
    sem_t           sem;                        /* process-shared semaphore (Linux) */
    pthread_mutex_t lock;                       /* process-shared mutex (macOS / fallback) */
    int             use_sem;                    /* 1 if using POSIX semaphore */
    int             count;                      /* total commands recorded */
    int             head;                       /* next write index (ring) */
    HistoryEntry    entries[HISTORY_CAPACITY];   /* 10,000-command ring buffer */
} SharedState;
```

* **Persistence:** Saved on disk at `~/.terminator_history` (fallback `./.terminator_history`, override `$TERMINATOR_HISTORY_FILE`).
* **Limits:** Retains the full 10,000 commands (`HISTORY_CAPACITY = 10000`). The `history` command prints the most recent 1,000 entries (`HISTORY_DISPLAY = 1000`) with 1-based indices.
* **LCS Dynamic Programming (`Ctrl+R`):**
  1. *Exact Match:* Checks newest-to-oldest for exact match.
  2. *Substring DP Matrix:* Evaluates $L[i][j] = (S[i-1] == H[j-1]) \,?\, L[i-1][j-1] + 1 : 0$.
  3. If $\max L[i][j] > 2$, all commands sharing maximum match length are displayed in reverse chronological order; otherwise prints `No match for search term in history`.

### 5.2 Auto-Completion & Longest Common Prefix (LCP)
When <kbd>Tab</kbd> is pressed, the prefix before the cursor is resolved:
1. Target directory is scanned via `opendir()`/`readdir()`, filtered, and sorted with `qsort()`.
2. Matches are extended to their Longest Common Prefix:
   $$LCP_k = \max \{ l \mid \text{match}_i[0..l-1] == \text{match}_j[0..l-1] \; \forall i, j \}$$
3. **UX Behavior:**
   * *Single Match:* Auto-completes inline with a trailing space or slash.
   * *Ambiguous Menu:* Extends to LCP. If still ambiguous, displays a numbered list (`1. fileA   2. fileB`) below the cursor.
   * *Immediate Selection:* The user selects an option by pressing the single number key (`1`..`9`) **without pressing Enter**.
   * *More than 9 Matches:* Scans up to 32 matches (`AUTOCOMPLETE_MAX_MATCHES`). The first 9 are directly selectable; pressing any non-digit key, BackSpace, or Esc dismisses the menu and processes the keystroke immediately.

---

## 6. Signal Handling & Event Interception

Terminator intercepts keystrokes at the X11 protocol level rather than relying on terminal tty line disciplines:
```
User Keystroke (Ctrl+C / Ctrl+Z) ──> X11 Event ──> GUI: handle_keypress()
   ──proto_send(GUI_MSG_SIGNAL)──> Tab Worker ──> kill(-pgid, sig) ──> Process Group
```
1. **Terminal Immunity:** Terminator ignores `SIGPIPE` and intercepts `Ctrl+C`/`Ctrl+Z` as X11 events, ensuring the terminal emulator itself never crashes or suspends.
2. **Targeted Delivery:** Signals are forwarded across IPC pipes to the worker, which issues `kill(-pgid, sig)` exclusively to the active child process group.

---

## 7. Bash Script Interpreter & Syntax Validator

Terminator executes `.sh` scripts internally without spawning `/bin/bash`:
* **Pre-Run Syntax Validation:** `script_validator_check_file()` parses quote balance and verifies balanced control flow (`if` $\to$ `then` $\to$ `fi`). Syntax errors immediately abort with exit code `2`.
* **State Machine Execution:** Evaluates nested `if/elif/else/fi` blocks using a stack (`COND_ACTIVE`, `COND_SKIP_BRANCH`, `COND_SKIP_TO_FI`).
* **POSIX `[ ... ]` Evaluator:** Supports file tests (`-f`, `-d`, `-e`, `-s`), string comparisons (`=`, `!=`, `-z`, `-n`), integer comparisons (`-eq`, `-ne`, `-gt`, `-lt`, `-ge`, `-le`), and logical operators (`!`, `-a`, `-o`).
* **Execution Context:** `source` executes inside the tab's active context; `./script.sh` runs inside an isolated sub-context with positional parameters `$0`, `$1`..`$#`, and `$?`.

---

## 8. Hotkey Customization & Keybinding System

Terminator provides runtime keybinding customization without recompilation:
* **Configuration File (`~/.terminatorrc`):** Checked on launch (fallback to defaults if absent). A starter template is provided at `.terminatorrc.sample`.
* **Syntax:** Declarative lines formatted as `action_name = Key_Combination`.
  * *Actions:* `move_left`, `move_right`, `move_home`, `move_end`, `history_prev`, `history_next`, `search_history`, `delete_to_start`, `delete_to_end`, `delete_prev_word`, `clear_screen`, `exit_or_eof`, `copy`, `paste`.
  * *Modifiers & Keys:* `Ctrl`, `Shift`, `Alt`, `Cmd` combined with keys like `Left`, `Right`, `Up`, `Down`, `Home`, `End`, `BackSpace`, `Delete`, `Enter`, `Tab`, `Space`, `Esc`, or alphanumeric characters.
* **Conflict Fallback:** Duplicate mappings or syntax errors log a warning and safely fall back to built-in defaults without breaking terminal usability.
