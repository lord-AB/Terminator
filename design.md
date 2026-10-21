# **Terminator: A Custom Terminal** 

**Computing Lab (CS69201) · Term Project** 

**Deadline: November 3, 2026, 11:59 PM** 

## **1. Overview** 

Build **Terminator** , a terminal emulator and shell in  C for Linux. It draws its own X11 window with multiple tabs and runs commands with pipes, redirection, job control, history, auto-completion and bash scripts. The project exercises the system calls covered in the course <mark>( fork , execvp , wait , pipe</mark> , <mark>dup2 , open / read / write , select/ poll , signal/ kill )</mark> ,  together with POSIX threads, semaphores and shared memory. 

In the examples, <mark>user@terminator></mark> is the prompt,  >  is the continuation prompt, and other lines are output. Anything this document does not specify is your design choice; explain it in the design document. 

## **2. Execution modes** 

Each tab is an independent shell. How tabs run depends on how Terminator is launched: 

<mark>./terminator                    # thread mode (default) ./terminator useProcess=True    # process mode</mark> 

In both modes, the commands a user types run as child processes <mark>( fork()</mark> + <mark>execvp() )</mark> . 

### **Thread mode (default)** 

- One process; each tab is a thread. The tabs share the process's memory. Protect shared state such as history with a mutex or semaphore. 

### **Process mode (** **<mark>useProcess=True )</mark>** 

- The GUI runs in the parent; each new tab <mark>fork() s</mark> its  own process and talks to the GUI through pipes. 

- History stays shared: keep it in shared memory, protected by a process-shared semaphore. 

- Closing or crashing one tab must not affect the others. 

**Both modes must behave the same.** A command typed in  one tab appears in another tab's history; for example  cd  in one tab does not change another  tab's directory; Ctrl+C in one tab does not touch another tab's jobs. How the GUI and tabs communicate, and how shared data is laid out, is up to you. In both modes the GUI is the only part that touches X11, and history lives in one shared, locked place. 

## **3. Task 1 — X11 GUI and tabs** 

Render the terminal in an X11 window with Xlib <mark>( XOpenDisplay , XCreateSimpleWindow</mark> , <mark>XNextEvent , XDrawString )</mark> . It should behave like a bash terminal:  prompt, typed input, command output and scrolling. All input and output go through the window, not the console. 

- Keep a text buffer per tab and redraw the window from it. 

- Turn <mark>KeyPress</mark> events into characters <mark>( XLookupString )</mark> . 

- Capture command output through pipes and draw it in the tab. 

- Tabs can be opened, closed and switched (by keyboard and/or mouse). A tab keeps running while another is shown. 

- The event loop must not busy-wait. 

<mark>[ 1: ~/Downloads ] [ 2: /tmp ] [ + ] user@terminator> ls abc.txt  def.txt  myprog.c</mark> 

## **4. Task 2 — Built-in and external commands** 

External commands run in a child with <mark>fork()</mark> + <mark>execvp() ,</mark> and the shell waits for them. Commands that change the shell's own state must be built-ins: at least  cd , <mark>exit , history, multiWatch , jobs/</mark> fg / bg  and <mark>source .</mark> An unknown command prints an error and the  tab stays usable. Keep the last exit status in  $? . 

<mark>user@terminator> cd ~/Downloads/mydir user@terminator> gcc -o myprog myprog.c user@terminator> ./myprog user@terminator> notacommand terminator: notacommand: command not found</mark> 

## **5. Task 3 — Multiline input and quoting** 

If a command is incomplete when Enter is pressed, show the  > prompt and keep reading. Support: 

- line continuation with a trailing  \ ; 

- single and double quotes that span several lines; 

- command substitution with <mark>$(...)</mark> and backticks, including  nested and multi-line ones (innermost runs first); 

- bash quoting rules: single quotes are literal, double quotes still expand <mark>$VAR</mark> and substitutions. 

<mark>user@terminator> echo "Isn't process creation exciting?\ > Fork, exec, repeat!" Isn't process creation exciting? Fork, exec, repeat! user@terminator> echo "Files in $(basename $(pwd)): $(ls | \ > wc -l)" Files in mydir: 3 user@terminator> echo 'literal: $HOME' "expanded: $HOME" literal: $HOME expanded: /home/user</mark> 

## **6. Task 4 — I/O redirection** 

Support  < ,  >  and both together, using <mark>dup()/ dup2()</mark> in the child. Report a missing input file instead of running the command. Background jobs that read the same file at the same time must not interfere with each other. 

<mark>user@terminator> sort < somefile.txt user@terminator> ls > abc user@terminator> ./a.out < infile.txt > outfile.txt</mark> 

## **7. Task 5 — Pipes** 

Support pipelines of any length with <mark>pipe() :</mark> N commands  need N−1 pipes and N child processes. Pipes can be combined with redirection. 

<mark>user@terminator> ls *.txt | wc -l user@terminator> cat abc.c | sort | head -5 user@terminator> ls *.txt | xargs rm</mark> 

## **8. Task 6 —  multiWatch** 

<mark>multiWatch ["cmd1", "cmd2", ...]</mark> runs all the commands  in parallel, one process each, and prints each command's output as it arrives until the user presses Ctrl+C. (This differs from <mark>watch "cmd1 && cmd2" ,</mark> which runs them one after another.) 

- Use one pipe per command and wait on all read ends with <mark>select()</mark> or <mark>poll();</mark> no busy-waiting. 

- Like <mark>watch ,</mark> re-run a command after it finishes (every  2 s by default). 

- Ctrl+C, detected in the X11 event loop, must terminate every child (e.g. with <mark>kill() )</mark> ; the shell keeps running. 

Each piece of output is printed in this format (marks are deducted otherwise): 

<mark>"cmd1", <unix_timestamp>:</mark> 

<mark>---------------------------------------------------<output of cmd1> ----------------------------------------------------</mark> 

<mark>user@terminator> multiWatch ["date +%T", "ls | wc -l"] "date +%T", 1790860020: ---------------------------------------------------18:37:00 ---------------------------------------------------"ls | wc -l", 1790860020: ---------------------------------------------------3 ---------------------------------------------------^C user@terminator></mark> 

## **9. Task 7 — Hotkeys** 

Support the bash-style editing keys below while the user types a command. Detect them in the KeyPress handler (check ControlMask / ShiftMask), update the input buffer and cursor, then redraw. 

The keys in the table are defaults. Users must be able to change them, for example through a config file such as ~/.terminatorrc, a built-in command, or a settings prompt. You choose the mechanism. Custom bindings take effect without recompiling, and are still in place after Terminator restarts. If a binding is invalid or conflicts with another one, report it and fall back to the default. 

|Key|Action|
|---|---|
|←/→|Move within the line|
|Ctrl+A / Ctrl+E|Move to the start / end of the line|
|↑/↓|Browse command history|
|Ctrl+U / Ctrl+K|Delete from the cursor to the start / end of the<br>line|
|Ctrl+W|Delete the previous word|
|Ctrl+L|Clear the screen|
|Ctrl+D|Exit the shell (close the tab) / send EOF|
|Ctrl+Shift+C / Ctrl+Shift+V|Copy / paste, using X11 selections|



## **10. Task 8 — Signals and job control** 

- **Ctrl+C** stops the running command and the prompt returns.  Terminator itself must never stop. 

- **Ctrl+Z** suspends the running command and the prompt  returns. 

- Support <mark>cmd &</mark> to start a job in the background, and <mark>jobs ,</mark> fg ,  bg  to manage jobs. 

Keys arrive as X11 events, so the shell must deliver the signals itself with <mark>kill() .</mark> 

<mark>user@terminator> sleep 100 ^Z [1]+  Stopped                 sleep 100 user@terminator> bg %1 [1]+ sleep 100 & user@terminator> fg %1 sleep 100 ^C user@terminator></mark> 

## **11. Task 9 — Searchable history** 

- Keep the last 10,000 commands in a history file (as bash does with <mark>~/.bash_history )</mark> , shared by all tabs. 

- <mark>History command</mark> shows the most recent 1,000 commands. 

- **Ctrl+R** shows <mark>Enter search term:  .</mark> On Enter, print: 

   - a.  the most recent command that exactly matches the term; otherwise 

   - b.  the command(s) sharing the longest common substring with the term, if that match is longer than 2 characters; otherwise 

   - c. <mark>No match for search term in history.</mark> 

<mark>user@terminator> (Ctrl+R) Enter search term: myprog ./myprog < in.txt gcc -o myprog myprog.c user@terminator> (Ctrl+R) Enter search term: xyz No match for search term in history</mark> 

## **12. Task 10 — File-name auto-completion** 

Pressing **Tab** completes the word before the cursor  using the files in the tab's current directory. The rest of the line stays as typed, even if the command does not exist. 

- One match: complete it. 

- Several matches: extend to their longest common prefix; if still ambiguous, show a numbered list and let the user pick. 

- No match: do nothing. 

With <mark>abc.txt , abcd.txt</mark> and <mark>def.txt</mark> in the directory: 

<mark>user@terminator> ./myprog de            ← Tab user@terminator> ./myprog def.txt user@terminator> ./myprog def.txt abc   ← Tab 1. abc.txt   2. abcd.txt                ← press 1 user@terminator> ./myprog def.txt abc.txt</mark> 

## **13. Task 11 — Running bash scripts** 

Terminator must run bash script files <mark>( ./script.sh , bash script.sh , source script.sh )</mark> by interpreting  them itself, not by handing them to <mark>/bin/bash .</mark> Scripts  may contain: 

- comments, multiple commands, pipes, redirection and command substitution; 

- variables <mark>( NAME=value , $NAME )</mark> , arguments ( $1 ,  $2 ,  …,  $# ) and  $? ; 

- if  / <mark>elif</mark> / <mark>else</mark> /  fi , possibly nested, with <mark>[ ...  ]</mark> tests (e.g. <mark>-eq ,</mark> -gt ,  = ,  -f ,  -d ); 

- <mark>exit [n] .</mark> 

Loops and functions are not required. A syntax error (such as a missing  fi ) is reported instead of running the script. 

<mark>user@terminator> cat check.sh #!/bin/bash if [ -f "$1" ]; then echo "File exists, compiling..." gcc "$1" -o myprog else echo "Source file missing!" fi user@terminator> ./check.sh myprog.c File exists, compiling... user@terminator> bash check.sh nothere.c Source file missing!</mark> 

## **14. Deliverables** 

Submit your GitHub repository link through the form that will be posted on Moodle. The repository must contain: 

1. Complete source code for Terminator. 

2. A Makefile. Running `make` must build `./terminator` without errors, using the required X11 compiler flags. 

3. A README that explains:what the project is;the dependencies and how to install them (e.g. `libx11-dev`); how to build and run Terminator in both modes (`./terminator` and `./terminator useProcess=True`); which features are implemented and any known limitations and any other information that you feel necessary 

4. A design document covering: the X11 GUI and event loop; thread mode vs process mode, and how shared state is synchronised;process and job management;IPC (pipes, `dup2`, redirection);the data structures used for history and auto-completion;signal handling; any other design decisions. 

5. Anything else you think helps, such as test scripts, sample config files, demo commands or screenshots. 

