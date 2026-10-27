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

---

## 1. X11 GUI & Non-Blocking `poll()` Event Loop

Terminator does not rely on third-party terminal widget toolkits (such as VTE). It implements a custom, hardware-accelerated X11 window using low-level `libX11` primitives.

```
+-------------------------------------------------------------------+
|  [ 1: ~/project $ ] [ 2: /tmp ] [ + ]             (Tab Bar Area)  |
+-------------------------------------------------------------------+
|  user@terminator> ls -la                                          |
|  total 560                                                        |
|  -rw-r--r--  1 user  staff   6878 Oct 22 17:28 Makefile           |
|  -rw-r--r--  1 user  staff  11470 Oct 21 20:49 design.md          |
|  drwxr-xr-x 11 user  staff    352 Oct 22 02:45 src                |
|                                                                   |
|  user@terminator> ./myprog def.txt abc_                           |
|  +---------------------------------------+                        |
|  | 1. abc.txt   2. abcd.txt              |  (Dynamic Suggestions) |
|  +---------------------------------------+                        |
+-------------------------------------------------------------------+
```

### 1.1 Window & Graphic Context Lifecycle
1. **Connection & Visual Setup:** `gui_init()` opens a connection to the X server (`XOpenDisplay`). It queries the default screen, root window, and creates an unbacked top-level window with `XCreateWindow` selecting input events:
   - `ExposureMask` (window damage and initial draw)
   - `KeyPressMask` / `KeyReleaseMask` (keyboard input)
   - `ButtonPressMask` / `ButtonReleaseMask` (mouse tab clicks: Left-click switches tab or creates new tab via `[+]`; Middle-click or Right-click on a tab header closes that tab)
   - `StructureNotifyMask` (resizing and window closing)
2. **Double-Buffering Architecture:** To prevent visible flicker during fast command outputs and typing, Terminator draws exclusively into an off-screen `Pixmap` back-buffer allocated with `XCreatePixmap`. During `gui_redraw()`, the background, tab headers, scrollback lines, prompt, input text, and cursor block are drawn into the back-buffer, which is then atomically copied to the window using `XCopyArea` followed by `XFlush`.
3. **Typography & Monospace Grid:** Monospace font metrics (`XLoadQueryFont`) establish standard character cells:
   $$\text{char\_width} = \text{font}->\text{max\_bounds.width}, \quad \text{char\_height} = \text{font}->\text{ascent} + \text{font}->\text{descent}$$
   The content dimensions are dynamically recalculated upon `ConfigureNotify` events:
   $$\text{term\_cols} = \frac{\text{win\_width} - 8}{\text{char\_width}}, \quad \text{term\_rows} = \frac{\text{win\_height} - \text{bar\_height}}{\text{char\_height}}$$

### 1.2 Unified Non-Blocking `poll()` Loop
Rather than blocking inside `XNextEvent`, Terminator's GUI event loop monitors both X11 server traffic and worker pipes in a single `poll()` syscall:

```c
struct pollfd pfds[MAX_TABS + 1];
pfds[0].fd = ConnectionNumber(gui->display);
pfds[0].events = POLLIN;

for (int i = 0; i < MAX_TABS; i++) {
    if (gui->tabs[i].in_use) {
        pfds[nfds].fd = gui->tabs[i].worker.gui_read_fd;
        pfds[nfds].events = POLLIN;
        ...
    }
}
poll(pfds, nfds, 10); /* 10ms tick for responsive UI */
```

- When `pfds[0]` is readable: All pending X events are drained non-blockingly using `XPending(dpy)` and dispatched to `handle_keypress`, `handle_buttonpress`, or resize handlers.
- When `pfds[tab]` is readable: Output data from tab worker pipes is received via `proto_recv()`, appended to the tab's `ScreenBuf`, and redrawn.

---

## 2. Concurrency Architecture: Thread Mode vs. Process Mode

Terminator supports two execution paradigms, controlled by the `useProcess=True` CLI argument:

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
                      |   POSIX Shared Memory       |
                      |   /terminator_shm_<pid>     |
                      |   - 10,000 Command History  |
                      |   - Process-Shared sem_t    |
                      +-----------------------------+
```

### 2.1 Thread Mode (Default)
- **Model:** The main process runs the X11 GUI on the main thread and creates a dedicated POSIX thread (`pthread_create`) for each active tab.
- **Virtual Working Directory (`cwd` Isolation):** In Linux/UNIX, the kernel working directory (`chdir`) is process-wide. If one thread called `chdir()`, all other tabs would unexpectedly shift directories. To solve this, Terminator maintains a virtual working directory string in each tab's `ShellContext`:
  - When the user runs `cd <path>`, Terminator validates and canonicalizes the path using `realpath()` and updates `ctx->cwd` without modifying the process's working directory.
  - When child processes are spawned for command execution, the forked child invokes `chdir(ctx->cwd)` immediately prior to calling `execvp()`.
  
### 2.2 Process Mode (`useProcess=True`)
- **Model:** Each tab worker is instantiated as an independent child process via `fork()`.
- **CWD Isolation:** Kernel-level isolation is naturally preserved across processes.

### 2.3 POSIX Shared Memory & Semaphore Synchronization
To share the 10,000-command command history across all tabs regardless of execution mode:
1. **Shared Memory Segment:** Created on startup using `shm_open()` with `O_CREAT | O_RDWR` and sized via `ftruncate()`:
   $$\text{shm\_size} = \text{sizeof(SharedState)} \approx 41\,\text{MB}$$
   Mapped into the address space of all processes via `mmap(..., MAP_SHARED, ...)`.
2. **Process-Shared Semaphore:** The shared state header contains a POSIX semaphore:
   ```c
   sem_init(&shm->lock, 1 /* pshared = true */, 1 /* initial value */);
   ```
   Setting `pshared = 1` guarantees that mutual exclusion works reliably across independent forked processes as well as threads. Every history append or read operation locks the region using `sem_wait(&shm->lock)` and unlocks via `sem_post(&shm->lock)`.

---

## 3. Process & Job Management

Every pipeline or standalone command entered by the user is executed in its own isolated process group.

```
       Tab Worker (Parent)
               |
               | fork()
               v
       +---------------------------------------------+
       | Process Group (PGID = PID_1)                |
       |                                             |
       |  [Child 1: cat]  ==pipe==>  [Child 2: wc]   |
       +---------------------------------------------+
```

### 3.1 Process Groups (`setpgid`)
When spawning an $N$-stage pipeline in `pipeline.c`:
1. The first child calls `setpgid(0, 0)`, setting its own PID as the process group ID (`pgid = pids[0]`).
2. Subsequent children call `setpgid(0, pgid)` to join the established process group.
3. The parent tab worker also calls `setpgid(pids[i], pgid)` immediately after `fork()` to eliminate race conditions before signal delivery.

### 3.2 Foreground Multiplexing & Signal Delivery
While a foreground pipeline is running, the tab worker does not block inside `waitpid()`. It executes a `poll()` loop multiplexing the command's stdout capture pipe and the GUI input pipe:
- **`Ctrl+C` Event:** Delivered across the GUI pipe as `GUI_MSG_SIGNAL` with `SIGINT`. The tab issues `kill(-pgid, SIGINT)` to the entire process group.
- **`Ctrl+Z` Event:** Delivered as `GUI_MSG_SIGNAL` with `SIGTSTP`. The tab issues `kill(-pgid, SIGTSTP)`.
- **Status Checking:** The worker polls with `waitpid(-pgid, &status, WNOHANG | WUNTRACED)`:
  - If `WIFSTOPPED(status)`: The job is added to the tab's Job Table as `STOPPED`, formatted as `[1]+ Stopped <cmd>`, and the shell returns to the prompt.

### 3.3 Background Execution & Built-ins (`jobs`, `fg`, `bg`)
- **Background Execution (`cmd &`):** The command is placed in the background without waiting. Terminator prints `[job_id] <pgid>` and returns to the prompt immediately.
- **Asynchronous Reaping:** On every prompt display or command submission, `jobs_check_status()` non-blockingly checks all background jobs via `waitpid(-1, &status, WNOHANG)` and prints completion notifications (`[1]+ Done <cmd>`).
- **`jobs`:** Iterates through the tab's job list and prints ID, state (`Running` / `Stopped`), and command line.
- **`bg %n`:** Sends `kill(-job->pgid, SIGCONT)`, updates state to `RUNNING`, and prints `[n]+ <cmd> &`.
- **`fg %n`:** Prints the command name, sends `kill(-job->pgid, SIGCONT)`, and enters `pipeline_wait_foreground` to monitor the process group until completion or suspension.

---

## 4. IPC Architecture & Pipeline Redirection

### 4.1 GUI $\leftrightarrow$ Tab IPC Protocol
Communication between the X11 GUI and each tab worker utilizes two unidirectional UNIX pipe pairs with a structured framing protocol:

```
+--------------------+---------------------+--------------------------+
|  Msg Type (4 bytes)|  Msg Len (4 bytes)  |  Payload (Msg Len bytes) |
+--------------------+---------------------+--------------------------+
```

| Message Type | Direction | Content / Purpose |
| :--- | :--- | :--- |
| `TAB_MSG_READY` | Tab $\to$ GUI | Worker thread/process initialized |
| `TAB_MSG_CWD` | Tab $\to$ GUI | Tab changed working directory (updates tab title) |
| `TAB_MSG_OUTPUT` | Tab $\to$ GUI | Stdout/stderr output chunks to append to screen |
| `TAB_MSG_EXIT` | Tab $\to$ GUI | Shell exited (`exit` command or EOF) |
| `GUI_MSG_LINE` | GUI $\to$ Tab | User committed command line to execute |
| `GUI_MSG_SIGNAL`| GUI $\to$ Tab | Forwarded signal (`SIGINT`, `SIGTSTP`) |
| `GUI_MSG_CLOSE` | GUI $\to$ Tab | Tab closed by user; worker should terminate |

`proto_send()` and `proto_recv()` guarantee full packet framing across stream boundaries, preventing partial read tearing.

### 4.2 Pipeline Orchestration & `dup2()` Binding
For an $N$-command pipeline:
1. $N-1$ inter-command pipes are created: `int pipes[N-1][2]`.
2. A dedicated capture pipe is created to route final stdout to the tab worker.
3. For each child process $i \in [0, N-1]$:
   - If $i > 0$: `dup2(pipes[i-1][0], STDIN_FILENO)`.
   - If $i < N-1$: `dup2(pipes[i][1], STDOUT_FILENO)`.
   - If $i == N-1$: `dup2(capture_pipe[1], STDOUT_FILENO)`.
4. **Redirection Overrides:** `setup_redirections()` is executed *after* default pipe wiring:
   - Input `<`: Opens file with `O_RDONLY` and binds to `STDIN_FILENO`. If the file does not exist, an error is printed and child exits before calling `execvp()`.
   - Output `>`: Opens file with `O_WRONLY | O_CREAT | O_TRUNC, 0644` and binds to `STDOUT_FILENO`.
   - Append `>>`: Opens file with `O_WRONLY | O_CREAT | O_APPEND, 0644` and binds to `STDOUT_FILENO`.
5. All pipe file descriptors are closed in the child after `dup2()`.
6. Parent closes all intermediate pipe descriptors so EOF cleanly propagates across stages.

---

## 5. Data Structures & Algorithms

### 5.1 Persistent Shared History & LCS Search
The shared memory segment maintains a rolling ring buffer storing up to 10,000 commands:
```c
typedef struct {
    char lines[SHM_HISTORY_MAX][LINE_MAXLEN]; /* 10,000 x 512 bytes */
    int  head;   /* write pointer (0..9999) */
    int  count;  /* total stored */
    int  total;  /* monotonic count */
} HistoryBuffer;
```

#### Longest Common Substring (LCS) Search
When the user triggers reverse history search via `Ctrl+R`, Terminator applies dynamic programming to identify matching commands:
1. **Exact Match First:** If an entry strictly equals the search query, it is returned immediately.
2. **Dynamic Programming Matrix:** For search term $S$ of length $m$ and history entry $H$ of length $n$:
   $$L[i][j] = \begin{cases} L[i-1][j-1] + 1 & \text{if } S[i-1] == H[j-1] \\ 0 & \text{otherwise} \end{cases}$$
3. The maximum contiguous match length $\max_{i,j} L[i][j]$ is evaluated across all 10,000 history entries.
4. If $\text{max\_len} > 2$, all commands achieving $\text{max\_len}$ are returned in reverse chronological order (newest first). Otherwise, Terminator reports: `No match for search term in history`.

### 5.2 Auto-Completion & Longest Common Prefix (LCP)
When <kbd>Tab</kbd> is pressed, Terminator inspects the word immediately preceding `cursor_pos`:
1. **Directory Scanning:** The word is partitioned into `dir_part` and `file_prefix`. The target directory is opened via `opendir()`, and matching entries are collected into an array using `readdir()`.
2. **Lexicographical Sort:** All matching entries are sorted using `qsort()` and `strcmp()`:
   $$\text{match}_0 \le \text{match}_1 \le \dots \le \text{match}_{K-1}$$
3. **Longest Common Prefix (LCP):**
   $$LCP_k = \max \{ l \mid \text{match}_i[0..l-1] == \text{match}_j[0..l-1] \; \forall i, j \}$$
4. **Interactive Disambiguation:**
   - Single match: Automatically completes inline.
   - Ambiguous matches: The input line is extended to the LCP. If still ambiguous, a formatted numbered menu (`1. abc.txt   2. abcd.txt`) is rendered directly beneath the cursor.
   - Immediate Dismissal: Typing any non-menu key immediately clears the suggestion popup and processes the keystroke.

---

## 6. Signal Handling & Event Interception

Terminator intercepts keystrokes at the X11 protocol level rather than relying on terminal line disciplines (termios/tty driver):

```
User Keystroke (Ctrl+C / Ctrl+Z)
        |
        v
X11 KeyPress Event  --->  x11_window.c: handle_keypress()
                               |
                               | proto_send(GUI_MSG_SIGNAL, SIGINT)
                               v
                       worker.c: Tab Worker Loop
                               |
                               | kill(-pgid, SIGINT)
                               v
                   Target Process Group (Child Processes)
```

1. **Terminal Immunity:** Because Terminator is an X11 client, pressing `Ctrl+C` or `Ctrl+Z` does not deliver a signal to the terminal emulator itself. Terminator ignores `SIGPIPE` (`signal(SIGPIPE, SIG_IGN)`) so broken pipes never terminate the shell.
2. **Targeted Delivery:** The GUI translates `Ctrl+C` into `SIGINT` and `Ctrl+Z` into `SIGTSTP`, dispatching a protocol packet to the active tab worker. The worker issues `kill(-pgid, sig)` directly to the foreground process group.
3. **Job Control Reaping:** When a child terminates or stops, the worker receives notification via `WNOHANG | WUNTRACED` without blocking UI interaction.

---

## 7. Bash Script Interpreter & Syntax Validator

Terminator includes a built-in bash script interpreter that executes `.sh` scripts directly without spawning `/bin/bash`.

### 7.1 Pre-Run Syntax Validation
Before executing any script statements, `script_validator_check_file()` parses the entire script:
- Verifies quote balance (`'...'` and `"..."`).
- Validates control flow nesting: every `if` must have a matching `then` and be closed by a matching `fi`.
- Rejects malformed scripts immediately with exit code `2` without executing side effects.

### 7.2 State-Machine Execution Engine
- **Stack-Based Branch Tracking:** Maintains a conditional execution stack (`COND_ACTIVE`, `COND_SKIP_BRANCH`, `COND_SKIP_TO_FI`) to support nested `if / elif / else / fi` constructs.
- **POSIX `[ ... ]` Condition Evaluator:** Evaluates file tests (`-f`, `-d`, `-e`, `-s`), string checks (`=`, `!=`, `-z`, `-n`), integer comparisons (`-eq`, `-ne`, `-gt`, `-lt`, `-ge`, `-le`), and logical combinations (`!`, `-a`, `-o`).
- **Context Isolation:**
  - `source <script>`: Executes inside the tab's active `ShellContext`, modifying environment variables and working directory in place.
  - `./<script>` and `bash <script>`: Spawns an isolated child context inheriting environment variables and setting positional arguments `$0`, `$1..$#`, and `$#`.
