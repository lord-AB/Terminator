# Terminator: Multi-Tab X11 Terminal & Shell

Terminator is a multi-tab X11 terminal emulator and UNIX shell implemented in C from scratch using POSIX system calls and standard `libX11` primitives. It handles graphical window management, line editing, process orchestration, pipelines, job control, persistent shared history, auto-completion, and internal bash script interpretation without external shell dependencies.

---

## 1. Dependencies & Installation

Terminator requires GCC/Clang, GNU Make, POSIX threads, and X11 client development libraries.

### Linux (Ubuntu / Debian)
```bash
sudo apt-get update
sudo apt-get install -y build-essential libx11-dev
```

### Linux (Fedora / RHEL / CentOS)
```bash
sudo dnf install -y gcc make libX11-devel
```

### macOS
Terminator runs on macOS using XQuartz:
1. Install XQuartz and X11 development headers:
   ```bash
   brew install --cask xquartz
   brew install libx11 pkg-config
   ```
2. Start XQuartz (`open -a XQuartz`) and ensure `DISPLAY` is exported:
   ```bash
   export DISPLAY=:0
   ```

---

## 2. Build & Execution

### Compilation
Build the project using standard GNU Make:
```bash
make clean
make
```
The binary `./terminator` is compiled under `-Wall -Wextra -O2 -g -pthread` with zero compiler warnings.

### Execution Modes
Terminator supports two distinct tab concurrency models selectable via command-line arguments:

| Mode | Command | Concurrency Architecture |
| :--- | :--- | :--- |
| **Thread Mode** *(Default)* | `./terminator` | Each tab runs in an independent POSIX thread (`pthread_create`). Working directories are virtualized per tab so `cd` in one tab does not affect others. |
| **Process Mode** | `./terminator useProcess=True` | Each tab runs in an independent child process (`fork()`). Tabs share history via anonymous POSIX shared memory (`mmap`). |

*(Note: `useProcess=True`, `useProcess=true`, `useProcess`, and `use Process True` are all accepted case-insensitively).*

---

## 3. Implemented Features (Tasks 1 – 11)

| Task | Feature | Implementation Details |
| :--- | :--- | :--- |
| **1** | **X11 GUI & Tabs** | Double-buffered offscreen `Pixmap` rendering, custom tab bar (`[ 1: ~/dir ] [ + ]`), mouse click hit-testing, dynamic font metrics, and scrollback buffer. |
| **2** | **Built-ins & Exec** | Built-ins: `cd`, `exit`, `history`, `multiWatch`, `jobs`, `fg`, `bg`, `source`, `clear`. Non-builtins dispatched via `fork()` + `execvp()`. |
| **3** | **Multiline & Quotes** | Trailing `\` continuation, quote-aware lexer preserving literals in `'...'`, variable expansion in `"..."`, innermost-first command substitution `$(...)` and `` `...` ``. |
| **4** | **Redirection** | `<` file input, `>` overwrite output, `>>` append output via `dup2()`. Missing input files abort execution before spawning processes. |
| **5** | **Pipelines & Wildcards** | Arbitrary $N$-command pipelines (`cmd1 \| cmd2 \| ... \| cmdN`) with $N-1$ pipes. In-child glob expansion (`*`, `?`) evaluated after directory change. Exit status propagates from final stage. |
| **6** | **Parallel `multiWatch`** | Built-in command executing $K$ commands concurrently (`multiWatch ["cmd1", "cmd2"]`), non-blocking output multiplexing via `poll()`, 2-second periodic refresh, and child process group cleanup on `Ctrl+C`. Supports command chaining (`&&`, `||`, `;`). |
| **7** | **Hotkeys & Line Editing** | GNU Readline-style navigation (`Ctrl+A`/`E`, `Ctrl+U`/`K`, `Ctrl+W`, `Ctrl+L`, `Ctrl+D`), macOS `Cmd` modifier support, and custom mapping via `~/.terminatorrc`. X11 clipboard integration (`Ctrl+Shift+C` / `Ctrl+Shift+V`). |
| **8** | **Signals & Job Control** | Process group isolation (`setpgid`), X11 interception of `Ctrl+C` (`SIGINT`) and `Ctrl+Z` (`SIGTSTP`) forwarded to child PGID, background execution (`cmd &`), and job control built-ins (`jobs`, `fg %n`, `bg %n`). |
| **9** | **Searchable History** | Rolling 10,000-command ring buffer in POSIX shared memory and `~/.terminator_history`. `history` displays last 1,000 entries. `Ctrl+R` Longest Common Substring (LCS) dynamic programming search, cancellable via `Esc`/`Ctrl+C`. |
| **10** | **Auto-Completion** | <kbd>Tab</kbd> completion resolving to Longest Common Prefix (LCP). Ambiguous matches wrap across multiple downward lines. Instant single-keystroke selection (`1`..`9`) without requiring Enter. |
| **11** | **Bash Script Interpreter**| Interprets `.sh` scripts (`./script.sh`, `bash script.sh`, `source script.sh`) internally without calling `/bin/bash`. Features pre-execution AST syntax validation, `[ ... ]` condition evaluation (`-f`, `-d`, `=`, `!=`, `-eq`, etc.), nested `if/elif/else/fi`, `$0`..`$9`, `$#`, `$?`, and `exit [n]`. |

---

## 4. Keybindings & Configuration

Terminator loads keybindings on startup from `~/.terminatorrc`, `./terminatorrc`, or `./.terminatorrc` (a template is provided in `.terminatorrc.sample`).

### Default Keybindings
- **Tab Management:** `Ctrl+T` / `Cmd+T` (New Tab), `Alt+1`..`9` / `Cmd+1`..`9` (Switch Tab), `Ctrl+D` / `exit` / middle-click (Close Tab)
- **Cursor Navigation:** `Left` / `Right` (Char), `Ctrl+A` / `Home` (Start of line), `Ctrl+E` / `End` (End of line)
- **Line Editing:** `Ctrl+U` (Cut to start), `Ctrl+K` (Cut to end), `Ctrl+W` (Cut word), `Ctrl+L` (Clear screen)
- **History & Search:** `Up` / `Down` (Browse history), `Ctrl+R` (LCS search, `Esc` or `Ctrl+C` to cancel)
- **Completion:** `Tab` (LCP completion), `1`..`9` (Choose numbered match)
- **Clipboard:** `Ctrl+Shift+C` / `Cmd+C` (Copy), `Ctrl+Shift+V` / `Cmd+V` (Paste)
- **Job Control:** `Ctrl+C` (Interrupt foreground PGID), `Ctrl+Z` (Suspend foreground PGID)
- **Buffer Scroll:** Mouse wheel or `Shift+Up` / `Shift+Down` (scrolls back; entering a command snaps back to bottom)

---

## 5. Known Limitations

In compliance with the project specification:
1. **Scripting Language Scope:** Per Task 11, shell script interpretation is intentionally bounded to conditions (`if/elif/else/fi`), test brackets (`[ ... ]`), parameters, variables, pipes, redirections, and command substitutions. Loops (`for`, `while`) and function declarations are outside the specification.
2. **Terminal Protocol:** Terminator is an integrated graphical shell with a direct X11 text canvas. Full VT100 / curses alternate-screen terminal applications that require raw pseudo-terminal (pty) termios cursor addressing (such as `vim` or `htop`) are outside project scope.
3. **Background Job Stdin:** Background jobs launched with `&` have standard input redirected to `/dev/null` per POSIX conventions so they cannot steal input from the foreground shell.

---

## 6. Verification & Test Scripts

The repository includes automated scripts and sample commands for verification:

### Test Scripts
- `check.sh`: Exercises Task 11 condition evaluation (`if [ -f "$1" ]`), variable expansion, and compilation.
- `broken.sh`: Tests pre-execution syntax validation (reports syntax error without running).
- `test_script_exit.sh`: Tests script `exit [n]` status propagation into `$?`.
- `test_script_env.sh`: Tests variable expansion and isolated subshell environment.

### Sample Test Commands
```bash
# 1. Multi-stage pipelines & redirection (Tasks 4 & 5)
cat Makefile | grep -v '^#' | tr 'a-z' 'A-Z' | sort | head -n 5
echo "Terminator Test" > /tmp/term_test.txt && cat < /tmp/term_test.txt

# 2. Parallel multiWatch (Task 6)
multiWatch ["uptime", "cat Makefile | head -n 2"]
multiWatch ["sleep 2 && echo slow", "echo fast"]

# 3. Job control (Task 8)
sleep 60 &
jobs
sleep 30
# Press Ctrl+Z to suspend, then resume:
bg %1
fg %1
# Press Ctrl+C to interrupt

# 4. History search (Task 9)
# Press Ctrl+R, type search substring, press Enter (or Esc to cancel)
history

# 5. Bash script interpretation (Task 11)
./check.sh Makefile
bash broken.sh          # Aborts with syntax error
./test_script_exit.sh
echo $?                 # Displays 7
```
