# Cross-Platform Task Manager

A live **console process table** — PID, user, CPU%, memory, RSS, threads, state, and name — plus headline CPU and memory bars.

Written in **C99**. There is no JVM, no curses library, and no GUI toolkit. Each host (macOS, Linux, Windows) compiles exactly one collector; the dashboard only ever sees a portable `TaskSnapshot`. ANSI colour and an alternate-screen buffer are the whole "UI".

C is a better fit here than Pascal or Java for the same reason it was a better fit for the Resource Monitor: the interesting work is talking to the kernel. `proc_pidinfo`, `/proc/[pid]/stat`, and `CreateToolhelp32Snapshot` are C APIs. One compile-time host (`collect_darwin.c` / `collect_linux.c` / `collect_win.c`) keeps that noise out of the renderer, the same way the Resource Monitor hid Mach / `/proc` / Win32 behind a `ResourceSnapshot`.

The task manager:

- draws a live table that refreshes once a second (configurable) — windowed or fullscreen
- colours CPU numbers green / yellow / red at 60% and 85%
- needs **two samples** before CPU percentages mean anything (the first tick only stores counters)
- treats a pipe or `--once` as a single snapshot, not a live loop
- honours `NO_COLOR` and `--ascii` for terminals that still mangle UTF-8
- sorts by CPU (default), memory, PID, or name; `/` filters; `k` / `K` signal the selected row
- quits on `q` or Ctrl+C without leaving the terminal in raw mode

How the pieces fit together (same style as the Resource Monitor): `WORKINGS.md` for responsibilities and the counter math, `EXECUTION_FLOW.md` for a tick-by-tick trace.

## Requirements

- A **C99 compiler** on your `PATH` (`cc` / `gcc` / `clang` / MinGW)

macOS (Xcode command-line tools, already enough):

```bash
xcode-select --install   # only if `cc` is missing
```

Debian / Raspberry Pi OS (build **on** the Pi, not from macOS):

```bash
sudo apt install build-essential
```

`make` or `make linux` both work there. glibc hides POSIX APIs under strict `-std=c99`; `src/posix_features.h` asks for them (`sigaction`, `nanosleep`, `gethostname`, `kill`).

Windows: [w64devkit](https://github.com/skeeto/w64devkit), MinGW-w64, MSYS2, or any `gcc` that can see `windows.h`, `tlhelp32.h`, `psapi`, and `advapi32`. Plain `make` is enough — w64devkit's `uname` says `Windows` (not `MINGW*`), and the Makefile keys off that plus `OS=Windows_NT`.

## Run

From the project root:

```bash
make
make run
```

That compiles to `build/task-manager` (or `build/task-manager.exe` on Windows) and opens the live table. `q` quits; space pauses. `make windows` is the same host with the filename `TaskManager.exe`.

On **Linux / Raspberry Pi OS**, run the binary from a terminal. `open` is a macOS command; on the Pi it (or a file-manager double-click) starts the process with no TTY, so the live UI has nowhere to draw.

```bash
./build/task-manager          # live table (this window)
./build/task-manager --once   # one snapshot, then back to the prompt
make run
```

If you do click the binary on the desktop, it now tries to open `x-terminal-emulator` / `lxterminal` / `xterm` for you.

Or with Make on other OSes:

```bash
make linux      # Linux binary (run this on Linux)
make windows    # TaskManager.exe (explicit Windows host; plain `make` also works)
make test       # headless two-sample checks (no raw terminal)
make once       # one framed snapshot, then exit
make clean      # remove build/
```

Manual compile on macOS:

```bash
mkdir -p build
cc -std=c99 -Wall -Wextra -O2 -Isrc -o build/task-manager \
    src/taskman.c src/util.c src/term.c src/render.c src/collect_darwin.c
build/task-manager
```

## Using it

1. `make run`. The first visible frame is already primed (the process slept one interval before drawing).
2. The headline CPU bar is **percent of all cores** (the Resource Monitor definition). Each row's `CPU%` is **percent of one core** (Activity Monitor / `top`) — a busy compile can read `250.0`.
3. Memory in the header is "in use" the way Activity Monitor / `free` / Task Manager mean it. `MEM%` on a row is that process's RSS over physical RAM.
4. Arrow keys (or `j`) move the selection. `c` / `m` / `p` / `n` change the sort. `r` reverses it. `/` types a filter.
5. `k` asks before sending `SIGTERM` (Windows: `TerminateProcess`). `K` is `SIGKILL` / the same terminate, labelled KILL. pid 1 is refused on Unix.
6. `space` freezes collection (the clock in the footer still updates). `q` or Ctrl+C restores the terminal and exits.

Your own process is tinted cyan so you can see the dashboard itself in the list.

## Flags

| Flag | Effect |
|------|--------|
| `-1` / `--once` | Print one snapshot and exit. No alt-screen, no raw mode. |
| `-n` / `--interval SEC` | Refresh interval (default `1.0`, clamped to `0.2`–`60`) |
| `--sort KEY` | `cpu` (default), `mem`, `pid`, or `name` |
| `--filter SUB` | Case-insensitive substring on name, command, or user |
| `--ascii` | `#` / `.` bars and `>` instead of `█` / `░` / `▶` |
| `--no-color` | No ANSI colour. Also honours a non-empty `NO_COLOR`. |
| `-h` / `--help` | Usage text |

Piped to a file or to `less`, the process behaves like `--once` even without the flag. A live loop in a non-TTY is the wrong tool.

On **Windows cmd**, live frames are composed in memory and written once. Classic cmd is unbuffered, so the old “erase, then `printf` each line” path flashed every second. Windows Terminal is fine either way; `task-manager.exe` / `TaskManager.exe` should now sit still in both.

A **windowed** terminal (Mac Terminal, lxterminal on the Pi, a small cmd window) used to wrap the identity line and the memory annotation, which scrolled the CPU/memory bars off the top. Under 100 columns the header is a shorter line and the bars shrink to fit; fullscreen still gets the long form.

## Where it appears

| OS | Collector | What you see |
|----|-----------|--------------|
| **macOS** | `collect_darwin.c` | `sysctl(KERN_PROC_ALL)` + `proc_pidinfo`, Mach CPU ticks, Activity Monitor-style memory |
| **Linux** | `collect_linux.c` | `/proc/[pid]/stat` + `status` + `cmdline`, `/proc/stat`, `MemAvailable` |
| **Windows** | `collect_win.c` | Toolhelp (`PROCESSENTRY32`, not the `A` alias — w64devkit has none), `GetProcessTimes`, `GetProcessMemoryInfo`, `GetSystemTimes` |

Protected Windows processes that refuse `OpenProcess` still appear (name + thread count from Toolhelp) with CPU/RSS as `n/a`.

## Project layout

```
src/
  taskman.c           # program; parse flags, prime, live loop
  snapshot.h          # portable TaskSnapshot (the whole contract)
  collect.h           # collect_init / collect_snapshot / collect_signal
  collect_darwin.c    # macOS host
  collect_linux.c     # Linux host
  collect_win.c       # Windows host
  posix_features.h    # glibc C99: expose sigaction / nanosleep / kill
  render.c            # ANSI table (one fwrite per live frame)
  term.c              # raw mode, alt screen, key poll (including arrows)
  util.c              # sleep, byte formatting, case-insensitive filter
  taskmantest.c       # make test
Makefile
```
