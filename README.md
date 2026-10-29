# Terminator: A Custom Terminal & Shell

**Computing Laboratory (CS69201) · Term Project**  
*A multi-tab X11 terminal emulator and UNIX shell built in C from scratch.*

---

## 1. Overview

**Terminator** is an integrated terminal emulator and interactive shell developed in C for Linux and macOS. It opens its own X11 graphical window featuring a custom multi-tab bar, custom font rendering, and an interactive command-line interface. 

The terminal shell implements pipelines of arbitrary length, comprehensive I/O redirection, job control, persistent shared history across tabs with Longest Common Substring (LCS) search, auto-completion with Longest Common Prefix (LCP), bash-style line editing hotkeys with X11 clipboard integration, parallel command watching (`multiWatch`), and an internal bash script interpreter that executes `.sh` scripts without invoking `/bin/bash`.

---

## 2. Dependencies & Installation

### Linux (Ubuntu / Debian)
Install GCC, Make, POSIX pthread libraries, and X11 development headers:
```bash
sudo apt update
sudo apt install build-essential libx11-dev
```

### Linux (Fedora / RHEL)
```bash
sudo dnf install gcc make libX11-devel
```

### macOS
Terminator natively supports macOS using XQuartz or Homebrew:
1. Install [XQuartz](https://www.xquartz.org/) or install via Homebrew:
   ```bash
   brew install --cask xquartz
   brew install libx11 pkg-config
   ```
2. Start XQuartz (`open -a XQuartz`), enable *"Allow connections from network clients"* in XQuartz preferences if needed, and verify `DISPLAY` is set:
   ```bash
   export DISPLAY=:0
   ```

---

## 3. Building and Running

### Build
Run `make` from the repository root:
```bash
make clean
make
```
This builds the binary `./terminator` with standard `-Wall -Wextra -g -pthread` compiler flags and linked X11 libraries.

### Execution Modes

Terminator supports two distinct concurrency architectures:

#### 1. Thread Mode (Default)
In thread mode, Terminator runs as a single process where each terminal tab executes in its own POSIX thread (`pthread_create`). Tabs maintain virtualized working directories (`cwd`) so directory navigation in one tab never interferes with another:
```bash
./terminator
```

#### 2. Process Mode
In process mode, each terminal tab executes as an independent child process created via `fork()`:
```bash
./terminator useProcess=True
```

In both modes, user commands are spawned as child processes (`fork() + execvp()`).


---

## 4. Implemented Features

| Feature | Description |
| :--- | :--- |
| **Multi-Tab X11 GUI** | Custom double-buffered X11 window, dynamic tab bar `[ 1: ~/dir ] [ + ]`, mouse click hit-testing, dynamic font metrics. |
| **Concurrency Modes** | Thread Mode (per-tab pthread with virtual CWD isolation) and Process Mode (per-tab fork). |
| **Multiline & Quoting** | Line continuation with trailing `\`, quote-aware lexer preserving literal content in `'...'`, expanding variables in `"..."`, innermost-first `$(...)` and `` `...` `` command substitution. |
| **I/O Redirection** | File input `<` and output `>` / `>>` with proper descriptor binding via `dup2()`. Missing input files abort execution before spawning processes. |
| **Pipelines & Wildcards** | Arbitrary $N$-command pipelines (`cmd1 \| cmd2 \| ... \| cmdN`) with $N-1$ pipes. In-child globbing (`*`, `?`) evaluated after `chdir()`. Exit status propagates from the last pipeline stage. |
| **Parallel `multiWatch`** | Built-in command running $K$ commands in parallel, multiplexing output via non-blocking `poll()`, re-executing every 2 seconds, and cleanly terminating child process groups on `Ctrl+C`. |
| **Hotkeys & Keybindings** | Bash-style editing (`Ctrl+A`/`E`, `Ctrl+U`/`K`, `Ctrl+W`, `Ctrl+L`, `Ctrl+D`), macOS Command (`Cmd`) key support, conflict detection, and runtime configuration via `~/.terminatorrc`. |
| **X11 Clipboard** | Copy (`Ctrl+Shift+C`) and Paste (`Ctrl+Shift+V`) integrating with `XA_CLIPBOARD` via X11 selection requests and events. |
| **Signals & Job Control** | Process groups (`setpgid`), `Ctrl+C` (`SIGINT`), `Ctrl+Z` (`SIGTSTP`), background execution (`cmd &`), and job control built-ins (`jobs`, `fg %n`, `bg %n`). |
| **Searchable Shared History**| 10,000-command persistent history in POSIX shared memory and `~/.terminator_history`, `history` built-in, `Up`/`Down` browsing, and `Ctrl+R` Longest Common Substring (LCS) dynamic programming search. |
| **Auto-Completion** | Tab-key file-name completion extending to Longest Common Prefix (LCP), lexicographically sorted suggestions displayed directly beneath the typing line, and instant dismissal on non-menu keystrokes. |
| **Bash Script Interpreter** | Interprets scripts (`./script.sh`, `bash script.sh`, `source script.sh`) internally without calling `/bin/bash`. Features pre-execution syntax validation, `[ ... ]` condition evaluation (`-f`, `-d`, `=`, `!=`, `-eq`, etc.), control flow (`if/elif/else/fi`), `$0..$9`, `$#`, `$?`, and `exit [n]`. |

---

## 5. Hotkey Configuration (`~/.terminatorrc`)

Terminator automatically reads `~/.terminatorrc` on launch. A sample template is provided in `.terminatorrc.sample`.

```ini
# Copy template to home directory:
cp .terminatorrc.sample ~/.terminatorrc
```

### Default Keybindings & Mouse Actions
- **New Tab:** `Ctrl+T` / `Cmd+T` or Left-click on `[ + ]`
- **Switch Tabs:** `Alt+1` through `Alt+9` or Left-click on tab header
- **Close Tab:** `Ctrl+D`, `exit`, or Middle-click / Right-click on tab header
- **Move Cursor:** `Left` / `Right`
- **Start / End of Line:** `Ctrl+A` / `Ctrl+E`
- **Delete Line Before / After:** `Ctrl+U` / `Ctrl+K`
- **Delete Word:** `Ctrl+W`
- **Clear Screen:** `Ctrl+L`
- **Reverse History Search:** `Ctrl+R`
- **Copy Selection:** `Ctrl+Shift+C`
- **Paste Selection:** `Ctrl+Shift+V`
- **Interrupt / Suspend:** `Ctrl+C` / `Ctrl+Z`
- **Scroll Buffer:** `Shift+Up` / `Shift+Down` or Mouse Wheel (Button4/Button5)

---

## 6. Project Structure

```
terminator/
├── Makefile                        # Build system with dependency rules
├── README.md                       # Setup, usage, and architecture overview
├── design.md                       # Detailed engineering design document
├── .terminatorrc.sample            # Hotkey configuration template
│
├── include/                        # Subsystem headers
│   ├── common.h                    # Constants, types, macros
│   ├── verification.h              # Headless multi-stage integration verification engine
│   ├── gui/                        # X11 window, tab bar, screen buffer, clipboard
│   ├── ipc/                        # Protocol framing, worker threading/forking, SHM sync
│   ├── input/                      # Line editor, hotkeys, auto-completion
│   ├── parser/                     # Multiline detector, lexer, AST, variable & command expansion
│   ├── exec/                       # Shell context, pipelines, redirection, jobs, built-ins
│   ├── features/                   # History store, LCS search, multiWatch
│   └── script/                     # Script validator, condition evaluator, interpreter
│
├── src/                            # Implementation files mirroring include/
│   ├── main.c                      # Entry point, CLI flags, initialization
│   ├── verification.c              # 11-stage automated verification engine for headless mode
│   ├── gui/                        # X11 rendering and event dispatching
│   ├── ipc/                        # IPC pipes, worker threads/processes, SHM
│   ├── input/                      # Terminal input, auto-completion, hotkeys
│   ├── parser/                     # Lexing, AST construction, substitutions
│   ├── exec/                       # Execution engine, jobs, built-in commands
│   ├── features/                   # Multiwatch, history ring-buffer, LCS
│   └── script/                     # Bash script syntax validator and execution engine
│
└── tests/                          # Automated unit test suite
    ├── test_gui_buffer.c           # Screen buffer ring and scrolling tests
    ├── test_parser.c               # Lexer, quoting, and substitution tests
    ├── test_redirect.c             # Redirection and missing file checks
    ├── test_pipeline.c             # Pipeline orchestration and wildcard tests
    ├── test_multiwatch.c           # MultiWatch concurrency and polling tests
    ├── test_hotkeys.c              # Hotkey parsing and fallback tests
    ├── test_jobs.c                 # Process group and signal delivery tests
    ├── test_lcs.c                  # Longest Common Substring algorithm tests
    ├── test_autocomplete.c         # File completion, LCP, and menu tests
    ├── test_script.c               # Script interpreter and validator tests
    ├── gui_interactive_test.c      # Automated X11 event driver for GUI testing
    └── scripts/                    # Test scripts (including check.sh)
```
