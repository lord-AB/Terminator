# Terminator: System Design & Architecture Document

This document describes the internal engineering, concurrency architecture, process management, IPC protocols, data structures, and algorithms of Terminator.

---

## 1. X11 GUI & Non-Blocking Event Loop

Terminator is implemented as a standalone X11 application using low-level `libX11` client library primitives.

### 1.1 Double-Buffered Canvas & Layout
```
+--------------------------------------------------------------------------+
|  [ 1: ~/project $ ] [ 2: /tmp ] [ + ]                    (Tab Bar)       |
+--------------------------------------------------------------------------+
|  user@terminator> ls -la                                                 |
|  total 560                                                               |
|  -rw-r--r--  1 user  staff   6878 Nov  1 16:00 Makefile                  |
|  -rw-r--r--  1 user  staff  11470 Nov  1 16:00 README.md                 |
|                                                                          |
|  user@terminator> ./myprog def.txt abc_                                  |
|  +--------------------------------------------------------------------+  |
|  | 1. abc.txt   2. abcd.txt                              (Suggestions)|  |
|  +--------------------------------------------------------------------+  |
+--------------------------------------------------------------------------+
```

- **Direct Xlib Window:** `gui_init()` connects via `XOpenDisplay()` and creates an unbacked top-level window selecting `ExposureMask`, `KeyPressMask`, `ButtonPressMask`, and `StructureNotifyMask`.
- **Flicker-Free Double Buffering:** Rendering is performed entirely off-screen into a backing `Pixmap`. When a redraw is triggered, the buffer is transferred to the window using `XCopyArea()` followed by `XFlush()`.
- **Font & Grid Dimensions:** Monospace font metrics (`char_width`, `char_height`, `font_ascent`) are queried on launch via `XLoadQueryFont()`. Usable screen rows and columns are recalculated dynamically upon `ConfigureNotify` events:
  $$\text{cols} = \frac{\text{width} - 8}{\text{char\_width}}, \quad \text{rows} = \frac{\text{height} - \text{bar\_height}}{\text{char\_height}}$$
- **Mouse & Tab Interaction:**
  - *Click Hit-Testing:* Left-clicking a tab header switches tabs; clicking `[ + ]` spawns a new tab. Middle-clicking or right-clicking a tab header closes the tab.
  - *Scrollback Buffer:* Scrolling the mouse wheel (Button 4 / 5) or pressing `Shift+Up` / `Shift+Down` offsets the rendering window over the terminal history lines. Typing any keystroke or submitting a command immediately snaps the view to the bottom prompt.

### 1.2 Unified Non-Blocking `poll()` Loop
To maintain 60 FPS responsiveness without worker thread synchronization locks inside the GUI loop, a single `poll()` multiplexes X server socket traffic and tab IPC descriptors:

```c
struct pollfd pfds[MAX_TABS + 1];
pfds[0].fd = ConnectionNumber(gui->display);
pfds[0].events = POLLIN;

int nfds = 1;
for (int i = 0; i < MAX_TABS; i++) {
    if (gui->tabs[i].in_use) {
        pfds[nfds].fd = gui->tabs[i].worker.gui_read_fd;
        pfds[nfds++].events = POLLIN;
    }
}

int ready = poll(pfds, nfds, 10); /* 10ms tick */
```

1. **X11 Server Traffic (`pfds[0]`):** If `pfds[0].revents & POLLIN` is set, pending events are drained non-blockingly using `XPending(dpy)` and dispatched to `handle_keypress()`, `handle_button_press()`, or `handle_configure()`.
2. **Tab Worker Traffic (`pfds[1..N]`):** If a tab's IPC descriptor is ready, `proto_recv()` reads the framed protocol packet. Terminal output chunks are appended to the tab's `ScreenBuf` and scheduled for backbuffer blitting.

---

## 2. Concurrency Architecture: Thread Mode vs. Process Mode

Terminator supports two distinct tab execution models selected at invocation:

```
                            +-----------------------+
                            |   Terminator GUI      |
                            |   (Main Process)      |
                            +-----------+-----------+
                                        |
               +------------------------+------------------------+
               | Thread Mode (Default)                           | Process Mode
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

### 2.1 Tab Isolation Models
- **Thread Mode (`./terminator`):**
  - Each tab runs inside a POSIX thread (`pthread_create`).
  - *Virtual CWD Isolation:* In a multithreaded process, `chdir()` changes the current working directory process-wide. To prevent tabs from interfering with one another, each tab maintains a virtual path string (`ctx->cwd`). When spawning external commands, the forked child invokes `chdir(ctx->cwd)` immediately prior to calling `execvp()`.
- **Process Mode (`./terminator useProcess=True`):**
  - Each tab is spawned as an independent child process via `fork()`.
  - Kernel-level working directories and address spaces are fully isolated.

### 2.2 Shared Memory & Synchronization
All tabs share a persistent 10,000-command history buffer backed by `SharedState`:

- **Allocation:**
  - *Process Mode:* Allocated before tab creation using anonymous shared memory:
    ```c
    SharedState *shm = mmap(NULL, sizeof(SharedState),
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ```
    This eliminates file descriptor leaks and dangling files in `/dev/shm`.
  - *Thread Mode:* Allocated on the heap via `calloc()`.
- **Synchronization Primitives:**
  - *Linux:* POSIX unnamed semaphore `sem_t` initialized with `sem_init(&sem, 1, 1)` (`pshared = 1`).
  - *macOS / Fallback:* Process-shared recursive mutex initialized via `pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)`.
  - Access is wrapped through `shm_lock()` and `shm_unlock()` to ensure atomic ring-buffer appends and lookups.

---

## 3. Process & Job Management

Every foreground pipeline and background command is executed within a dedicated process group (`setpgid`).

```
       Tab Worker (Parent)
               | fork()
               v
       +---------------------------------------------+
       | Process Group (PGID = PID_1)                |
       |  [Child 1: cat]  ==pipe==>  [Child 2: wc]   |
       +---------------------------------------------+
```

### 3.1 Process Groups & Foreground Pipelines
1. **PGID Initialization:** The first child process of a pipeline calls `setpgid(0, 0)` so its PID becomes the process group leader. Subsequent pipeline children call `setpgid(0, first_pid)`. The parent also calls `setpgid(first_pid, first_pid)` to eliminate race conditions between child execution and signal forwarding.
2. **Foreground Multiplexing:** While a foreground pipeline runs, the tab worker does not block in a synchronous `wait()`. Instead, it reads child output descriptors and forwards output chunks to the GUI via IPC pipes, tracking process exit codes using `waitpid()`.

### 3.2 Background Jobs & Job Table
- **Asynchronous Launch (`cmd &`):** The tab worker marks the process group as background, redirects its standard input to `/dev/null`, prints `[job_id] <pgid>`, and immediately returns control to the prompt.
- **Job Table Tracking:** Jobs are registered in `Job jobs[MAX_JOBS]` tracking `job_id`, `pgid`, command string, and status (`JOB_RUNNING`, `JOB_STOPPED`, `JOB_DONE`).
- **Asynchronous Reaping:** `jobs_check_status()` is called before each prompt display and during loop ticks using `waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)`. Reaped background jobs print completion messages:
  ```
  [1]+ Done    sleep 10
  ```
- **Job Control Built-ins:**
  - `jobs`: Formats and lists active and suspended background jobs.
  - `fg %n`: Moves job $n$ to the foreground, connects signal monitoring, and waits for its exit or suspension.
  - `bg %n`: Resumes a suspended job in the background by sending `kill(-pgid, SIGCONT)`.

---

## 4. IPC Architecture & Pipeline Redirection

### 4.1 Framed Binary Protocol
Communication between the master GUI and each tab worker traverses a pair of unidirectional UNIX pipes (`gui_to_tab` and `tab_to_gui`). All messages use an 8-byte binary framing header:

```
+-----------------------------------+-----------------------------------+
|       uint32_t type (4 bytes)     |       uint32_t len (4 bytes)      |
+-----------------------------------+-----------------------------------+
|                   payload bytes (len bytes) ...                       |
+-----------------------------------------------------------------------+
```

| Message Constant | Flow Direction | Payload Content & Semantics |
| :--- | :--- | :--- |
| `TAB_MSG_READY` | Tab $\to$ GUI | Handshake confirming tab initialization. |
| `TAB_MSG_OUTPUT`| Tab $\to$ GUI | Stdout/stderr terminal text chunk to render. |
| `TAB_MSG_PROMPT`| Tab $\to$ GUI | Updated prompt string (e.g. `user@terminator:~$ `). |
| `TAB_MSG_CWD`   | Tab $\to$ GUI | Current directory string (updates tab title). |
| `TAB_MSG_CLEAR` | Tab $\to$ GUI | Screen clear notification (`Ctrl+L`). |
| `TAB_MSG_EXIT`  | Tab $\to$ GUI | Tab process termination with exit status. |
| `GUI_MSG_LINE`  | GUI $\to$ Tab | Committed command line string to parse and execute. |
| `GUI_MSG_SIGNAL`| GUI $\to$ Tab | Intercepted signal code (`SIGINT`, `SIGTSTP`). |
| `GUI_MSG_CLOSE` | GUI $\to$ Tab | Orderly tab termination request. |

`proto_send()` and `proto_recv()` loop over `read()`/`write()` calls until the exact 8-byte header and full payload are transferred, preventing message boundary tearing.

### 4.2 Pipeline Wiring & Redirection
For an arbitrary $N$-stage pipeline (`cmd1 | cmd2 | ... | cmdN`):
1. The shell creates $N-1$ pipes via `pipe()`.
2. For child $i \in [0, N-1]$:
   - If $i > 0$, standard input is bound to `pipes[i-1][0]` via `dup2()`.
   - If $i < N-1$, standard output is bound to `pipes[i][1]` via `dup2()`.
3. **Explicit Redirection Overrides:**
   - `< file`: Opened with `O_RDONLY`. If the file does not exist, an error is reported immediately and pipeline creation is aborted. Connected via `dup2(fd, STDIN_FILENO)`.
   - `> file`: Opened with `O_WRONLY | O_CREAT | O_TRUNC, 0644`. Connected via `dup2(fd, STDOUT_FILENO)`.
   - `>> file`: Opened with `O_WRONLY | O_CREAT | O_APPEND, 0644`. Connected via `dup2(fd, STDOUT_FILENO)`.
4. **Descriptor Hygiene:** All unused read and write pipe ends are immediately closed in both the parent and children. This ensures downstream commands receive `EOF` as soon as their upstream producer finishes.

---

## 5. Data Structures & Algorithms

### 5.1 History Ring Buffer & LCS Dynamic Programming
History is maintained in a shared ring buffer capacity of 10,000 entries:

```c
typedef struct {
    char cmd[MAX_CMD_LEN];      /* 4096-byte command buffer */
    int  valid;
} HistoryEntry;

typedef struct SharedState {
    sem_t           sem;
    pthread_mutex_t lock;
    int             use_sem;
    int             count;
    int             head;                       /* write cursor modulo 10000 */
    HistoryEntry    entries[HISTORY_CAPACITY];  /* 10,000 entries */
} SharedState;
```

#### Longest Common Substring (LCS) Algorithm (`Ctrl+R`)
When the user triggers reverse search (`Ctrl+R`), the search term $S$ of length $M$ is matched against candidate history strings $H$ of length $N$:

1. Exact match check: Scans backwards from newest to oldest.
2. Dynamic Programming Matrix: Computes maximum contiguous common substring length:
   $$L[i][j] = \begin{cases} L[i-1][j-1] + 1 & \text{if } S[i-1] == H[j-1] \\ 0 & \text{otherwise} \end{cases}$$
3. Complexity: Time complexity is $O(M \cdot N)$ per candidate command. Space is optimized using two rolling rows ($O(M)$ space).
4. Matches achieving the maximum length $\ge 3$ are returned in reverse chronological order. Pressing `Esc`, `Ctrl+C`, or `Ctrl+G` instantly aborts search mode and restores the original prompt.

### 5.2 Auto-Completion & Longest Common Prefix (LCP)
When <kbd>Tab</kbd> is pressed:
1. The prefix before the cursor is resolved into directory and base-name components.
2. The directory is scanned via `opendir()`/`readdir()`, filtering entries matching the prefix.
3. Candidate matches are sorted lexicographically using `qsort()`.
4. **Longest Common Prefix Reduction:**
   $$LCP = \max \{ k \mid \forall i, j: \text{match}_i[0..k-1] == \text{match}_j[0..k-1] \}$$
   If $LCP$ is longer than the current input prefix, the input buffer is automatically expanded to the common prefix.
5. **Multi-Line Downward Wrapping:**
   When multiple candidates remain, `compute_autocomplete_lines()` packs items into formatted columns (`1. fileA   2. fileB`). If the text exceeds the terminal column width, it wraps cleanly across subsequent downward lines rather than truncating horizontally.
6. **Zero-Enter Selection:** Pressing a number key `1`..`9` immediately selects the corresponding file without requiring the user to press Enter.

---

## 6. Signal Handling

Terminator isolates terminal GUI stability from child process signal disruptions:

```
User Keystroke (Ctrl+C / Ctrl+Z) ──> X11 KeyPress Event ──> GUI: handle_keypress()
   ──proto_send(GUI_MSG_SIGNAL)──> Tab Worker ──> kill(-pgid, sig) ──> Process Group
```

1. **X11 Key Interception:** Key combinations like `Ctrl+C` and `Ctrl+Z` are intercepted directly as X11 `KeyPress` events. They do not generate POSIX signals against the GUI process.
2. **Targeted Delivery to Process Groups:** When a signal event is intercepted, the GUI sends a `GUI_MSG_SIGNAL` IPC packet to the tab worker. The worker delivers the signal strictly to the foreground process group:
   ```c
   kill(-active_pgid, SIGINT);   /* Ctrl+C */
   kill(-active_pgid, SIGTSTP);  /* Ctrl+Z */
   ```
3. **Shell Immunity:** Because signals are targeted to negative PGIDs (`-pgid`), the GUI main process and the tab worker process never receive the signal, preventing unexpected termination or suspension. `SIGPIPE` is ignored process-wide (`signal(SIGPIPE, SIG_IGN)`).

---

## 7. Additional Design Decisions

### 7.1 Built-in Bash Script Interpreter (Task 11)
Rather than delegating execution to `/bin/bash`, Terminator implements an internal interpreter for `.sh` scripts:
- **Pre-Execution Syntax Validator:** Before executing any script line, `script_validator_check_file()` performs an AST validation pass. It verifies balanced double/single quotes and matching control structures (`if` $\to$ `then` $\to$ `fi`). If an error is detected (such as a missing `fi`), execution is refused and an error code `2` is returned immediately without running any commands.
- **Nested Condition State Machine:** Control flow is managed via a state machine stack supporting `if`, `elif`, `else`, `fi`:
  - `COND_ACTIVE`: Statements in this branch are parsed and executed.
  - `COND_SKIP_BRANCH`: Condition evaluated to false; statements are skipped until `elif`, `else`, or `fi`.
  - `COND_SKIP_TO_FI`: A previous condition evaluated to true; all remaining branches are skipped until `fi`.
- **POSIX `[ ... ]` Condition Evaluator:** Implements recursive expression parsing for:
  - File checks: `-f` (regular file), `-d` (directory), `-e` (exists), `-s` (non-empty).
  - String comparisons: `=`, `!=`, `-z` (empty), `-n` (non-empty).
  - Integer comparisons: `-eq`, `-ne`, `-gt`, `-lt`, `-ge`, `-le`.
  - Logical connectors: `!` (NOT), `-a` (AND), `-o` (OR).
- **Subshell Environment:** Scripts executed via `./script.sh` or `bash script.sh` execute in an isolated environment with local positional variables `$0`, `$1`..`$9`, `$#`, and `$?`. Scripts invoked via `source script.sh` execute directly in the tab's current context.

### 7.2 Quote-Aware Lexer & Recursive Substitutions
- **Lexer State Machine:** Categorizes input tokens into single-quoted literals `'...'` (where all characters including `$`, `\`, and spaces are literal), double-quoted strings `"..."` (where variables and command substitutions are expanded), and bare tokens.
- **Command Substitution:** Innermost-first evaluation supports both `$(cmd)` and backticks `` `cmd` ``. The command is executed through a subshell pipeline, its stdout captured into a buffer, trailing newlines stripped, and spliced back into the argument stream.

### 7.3 Hotkey Configuration Parser
Keybindings are loaded at startup from `~/.terminatorrc`, `./terminatorrc`, or `./.terminatorrc`:
- Configuration is parsed once on initialization into an internal dispatch table, avoiding file I/O or string parsing overhead during runtime typing.
- Invalid configurations or syntax errors output diagnostic warnings and fall back to built-in default bindings.
