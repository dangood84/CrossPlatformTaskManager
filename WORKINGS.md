# How the Task Manager works

This note is for an automation tester who wants to see how a small C console app is structured: where it starts, who owns state, who paints the table, and how two kernel counter reads become `CPU  16.8` on a row.

You do not need to be a Mach, `/proc`, or Win32 expert. The same ideas show up in many monitors: an entry point, a snapshot struct, a host that fills it, and a loop that sleeps then redraws.

There is **no ncurses**, **no Pascal host unit**, and **no Swing**. Each metric is a field on `TaskSnapshot`. A collector that cannot fill a field leaves it zero (or `-1` for "not yet"). The renderer prints what it is given.

This is a **timed loop**, not a GUI event loop. Nothing interesting happens until the interval elapses (or a key arrives). That is the same split as the Resource Monitor.

## Mental model

```
taskman.c main
  → [Linux, no TTY at all] re-exec in lxterminal / xterm, exit
  → collect_init                  # one of collect_darwin / linux / win
  → collect_snapshot              # prime: store counters, rates are junk
  → sleep(interval)
  → collect_snapshot              # now CPU % is real
  → qsort the process rows
  → if --once or stdout not a TTY: print once, exit
  → term_init (alt screen, raw stdin)
       loop
           collect_snapshot       # unless paused / typing a filter / confirming kill
           qsort
           render_dashboard       # one fwrite (home + frame + [J])
           term_poll_key(interval)
           q / Ctrl+C → restore terminal, exit
```

| Layer | File | Tester-friendly analogy |
|-------|------|-------------------------|
| Entry / flags | `taskman.c` | Test runner: `--once`, `--interval`, `--sort`, `--filter` |
| State | `snapshot.h` | Fixture: one struct, no OS types |
| Host | `collect_*.c` | The only file that opens `/proc` or Mach |
| View | `render.c` | Turns the struct into bars and a table |
| Terminal shell | `term.c` | Alt screen + "did the user hit q / ↑ / k?" |

Only **one** `collect_*.c` is linked. The other two are not compiled on that OS. Same compile-time host idea as the Resource Monitor.

---

## 1. Entry point and execution lifecycle

### Where `main` lives

The process entry point is `main` in `src/taskman.c`. It parses flags (no `getopt`, so the same file builds with MSVC / MinGW), calls `collect_init`, takes **two** snapshots, sorts, then either prints once or enters the live loop.

```c
collect_snapshot(snap);          /* counters only */
util_sleep_ms(interval_ms);
collect_snapshot(snap);          /* rates exist */
sort_procs(snap);
if (once || !term_is_tty())
    render_once(snap, &opt);
else {
    term_init();
    while (!g_stop) { ... }
}
```

The snapshot is **heap-allocated**. It holds up to 768 process rows (~180 KB). Raspberry Pi OS can give a smaller default stack than macOS; a stack `TaskSnapshot` would be a mysterious crash on first collect.

### Lifecycle, step by step (macOS — the reference host)

1. **The OS** starts `build/task-manager`.
2. **`main`** reads `argv`. Default interval is 1.0 s. `NO_COLOR` forces `--no-color`.
3. **`collect_init`** (Darwin) zeroes the previous-tick buffers. Nothing talks to Mach yet.
4. **First `collect_snapshot`** fills hostname, memory, and the process list immediately. System `cpu_total` is `-1`, each row's `cpu_pct` is `-1`, and `primed` is 0 — there is no previous tick to subtract.
5. **`util_sleep_ms`** waits one interval. `nanosleep` is restarted on `EINTR` so a stray signal does not shorten the prime.
6. **Second `collect_snapshot`** subtracts Mach tick counters and each process's `pti_total_user + pti_total_system`. `primed` is 1. `sample_dt` is the real elapsed time (about 1.0 s, or 0.25 s under `make test`).
7. **`qsort`** orders the rows (CPU descending by default).
8. Live mode: **`term_init`** switches the TTY to non-canonical input, hides the cursor, and enters the alternate screen. `atexit` + `SIGINT` both call `term_restore`.
9. Each loop iteration: optional collect + sort, `render_dashboard` (compose a `FrameBuf`, `\x1b[H`, `\x1b[K` per line, one `fwrite`, then `\x1b[J`), `term_poll_key` for up to one interval.
10. **`q`**, Ctrl+C, or `SIGTERM`: restore the previous screen and cooked input, then `collect_shutdown`.

### Two user journeys

**`--once` / `make once` / piped stdout:**

```
prime → sleep → snapshot → sort → render_once → exit 0
```

No `tcsetattr`. Safe to grep. This is the path a CI job should use.

**Live table (`make run`):**

```
prime → sleep → snapshot → sort → term_init → (collect → sort → paint → poll)* → restore
```

`space` flips `paused`. Collection stops; the last snapshot is redrawn so the footer can still show `PAUSED`. Typing a filter or confirming a kill also skips collect so a keystroke is not racing a one-second refresh.

**Linux desktop / `open` / `xdg-open`:** stdin, stdout, and stderr are all non-TTYs. `linux_spawn_in_terminal` forks `x-terminal-emulator` / `lxterminal` / `xterm` with `TASK_MANAGER_IN_TERM=1` so the child is the live dashboard. `open` is still a macOS command; the right invocation from a Pi shell is `./build/task-manager`.

### Why testers care

- **Compile-time host is the feature flag.** Automating "Mac numbers" vs "Linux numbers" is `make` vs `make linux` on that machine, not a CLI switch.
- **The first snapshot is a fixture, not a result.** A test that asserts `cpu_total >= 0` after one call will fail on purpose. `taskmantest.c` takes two.
- **Two different CPU scales live on one screen.** The headline bar is 0..100 of *all* cores. A row is 100% = one core and can exceed 100. Do not assert that the sum of row `cpu_pct` equals `cpu_total`.
- **Exit is process-level.** There is no daemon and no pid file. Closing the terminal sends `SIGHUP`/`SIGINT` and `term_restore` runs from `atexit` if we got that far.
- **`make test`** never opens the alt screen. Use that for "do we have a hostname, a non-zero `mem_total`, and our own pid in the list". Use the live window for colour, pause, sort, filter, and resize.

---

## 2. Main files and responsibilities

This is a **separation of snapshot vs paint**, not a framework.

### `taskman.c` — composition root

- Parses flags
- On Linux, if every stdio fd is detached, opens a real terminal and re-execs
- Primes the collector
- Owns pause, sort, filter, selection, and the kill-confirm flag
- Calls `collect_signal` after a `y` on the confirm line
- Does **not** read `/proc` or Mach itself

### `posix_features.h` — glibc C99

Must be the **first** include on Linux. `-std=c99` hides `sigaction`, `nanosleep`, `clock_gettime`, `gethostname`, and `kill` unless `_DEFAULT_SOURCE` / `_POSIX_C_SOURCE` are set *before* any system header. The macros are Linux-only: the same defines on Darwin can hide BSD helpers.

### `snapshot.h` — the contract

Holds *numbers*, not file descriptors:

- `cpu_total` — 0..100 of all cores, or `-1` before prime
- `mem_total` / `mem_used` / `mem_available`
- `process_count` — what the kernel reported
- `proc_n` / `procs[]` — what we stored (capped at `SNAP_MAX_PROCS` = 768)
- `truncated` — 1 if the kernel had more than we stored
- each `ProcSample`: pid, ppid, name, command, user, state, `cpu_pct`, RSS, threads, `start_time`, `readable`
- `primed`, `sample_dt`

Hosts never poke the renderer. The renderer never pokes the host.

### `collect_darwin.c` / `collect_linux.c` / `collect_win.c`

Each file exports the same five symbols (`collect_init`, `collect_shutdown`, `collect_snapshot`, `collect_host_name`, `collect_signal`). Static buffers remember the previous tick, keyed by **pid + start time** so a recycled PID does not inherit the previous tenant's CPU counters.

That is why `taskmantest.c` can link `util.c` + one collector and never mention ANSI.

### `render.c` — the table

- Picks bar width from `opt->cols` (from `TIOCGWINSZ` / `GetConsoleScreenBufferInfo`)
- Green < 60%, yellow < 85%, red otherwise (row CPU is coloured after clamping to 100 for the threshold only)
- `--ascii` swaps `█░▶` for `#.>`
- Live frames are a `FrameBuf`: home, erase-to-end-of-line on each row, **one** `fwrite`. cmd.exe (and the MinGW CRT) leave the console unbuffered, so a leading `\x1b[J` plus line-by-line `printf` used to flash every second.
- Filter, scroll, and the selected pid are *options*, not snapshot fields. The collector does not know you typed `/chrome`.
- Under 100 columns the identity line and footer shrink, and both bars are sized from the memory annotation so they cannot wrap. A line of exactly `cols` glyphs plus a newline wraps on macOS Terminal (the cursor is already on the next row); hlines stay one column short.

It does not know about Mach or `/proc`.

### `term.c` — the shell

- Unix: clear `ICANON` and `ECHO`, keep `ISIG` so Ctrl+C still raises `SIGINT`
- Windows: `ENABLE_VIRTUAL_TERMINAL_PROCESSING` + UTF-8 output code page
- `term_poll_key` is a `poll` / `_kbhit` with a timeout — that timeout **is** the refresh interval
- Arrow / PgUp / PgDn / Home / End come back as `TERM_KEY_*` (CSI on Unix, 0/224 pairs on Windows). A lone Esc waits 30 ms and then returns `TERM_KEY_ESC` so filter-cancel works.
- `term_size` reports the real window (floor 40×10, not 60×16). Pretending a windowed Mac terminal was 60 columns made us paint lines wider than the glass.

### `Makefile` — compile-time host

`uname -s` is `Darwin` / `Linux` as usual. On Windows, w64devkit prints `Windows` (not `MINGW64_NT-*`) and cmd sets `OS=Windows_NT`. Either of those selects `collect_win.c` and `-lpsapi -ladvapi32`. A plain `make` that misses both used to link no collector; `ld` then died on `collect_init`. `make windows` always links the Windows host.

### What is *not* a file

There is no config file, no preferences plist, no log. Digit widths and colour thresholds are constants in `render.c`. A test that wants a red CPU number has to actually load a core; there is no `--fake-cpu=90`.

---

## 3. How counters become a percentage

This is **not** "read a `%` from the kernel". The kernel gives **monotonic counters**. The host subtracts.

```
this_total - prev_total  = ticks (or nanoseconds) in the interval
this_idle  - prev_idle   = idle ticks in the interval
busy / total * 100       = system utilisation (headline bar)
process_delta / dt * 100 = per-process % of one core
```

### System CPU (headline bar)

| Host | Counter source | Idle definition |
|------|----------------|-----------------|
| macOS | `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` | `CPU_STATE_IDLE` |
| Linux | `/proc/stat` first `cpu` line | `idle + iowait` |
| Windows | `GetSystemTimes` | `IdleTime`; kernel time **includes** idle |

`cpu_total == -1` is the "warming up" signal. After prime it is clamped to 0..100.

### Per-process CPU (table rows)

| Host | Lifetime counter | Units | Formula after prime |
|------|------------------|-------|---------------------|
| macOS | `pti_total_user + pti_total_system` | nanoseconds | `100 * delta_ns / 1e9 / dt` |
| Linux | `utime + stime` from `/proc/[pid]/stat` | clock ticks (`sysconf(_SC_CLK_TCK)`) | `100 * (delta_ticks / CLK_TCK) / dt` |
| Windows | `GetProcessTimes` kernel + user | 100-ns FILETIME | `100 * (delta / 1e7) / dt` |

A process that appeared this tick has no previous row → `cpu_pct = 0` once primed (and `-1` on the very first snapshot).

**PID reuse:** the previous-tick table is keyed by `pid + start_time` (`pbi_start_tvsec` / `/proc` starttime / `GetProcessTimes` creation). A new process in a recycled slot starts at 0% instead of flashing several thousand percent.

### Memory

| Host | Header "used" means | Row RSS means |
|------|---------------------|---------------|
| macOS | `(internal - purgeable) + wired + compressor` × page size | `pti_resident_size` |
| Linux | `MemTotal - MemAvailable` (kB × 1024) | `VmRSS` from `/proc/[pid]/status` |
| Windows | `ullTotalPhys - ullAvailPhys` | `WorkingSetSize` |

If a tester compares the header bar to Activity Monitor / Task Manager and they disagree by a gigabyte, check which definition that tool is using before filing a bug.

### Process list

| Host | Enumeration | Name / command | User | State |
|------|-------------|----------------|------|-------|
| macOS | `sysctl(KERN_PROC_ALL)` | `p_comm`, upgraded with `pbi_name` when `PROC_PIDTBSDINFO` works; `proc_pidpath` for the command | `getpwuid(e_ucred.cr_uid)` | `p_stat` `SIDL/SRUN/SSLEEP/SSTOP/SZOMB` → `I/R/S/T/Z` |
| Linux | `/proc` digit directories | `stat` comm + `cmdline` (NULs → spaces). Kernel threads have an empty cmdline, so we keep comm. | `Uid:` in `status` | the single `stat` letter |
| Windows | `CreateToolhelp32Snapshot` | `szExeFile` + `QueryFullProcessImageNameA` | token → `LookupAccountSidA` | `"R"`; `"I"` for pid 0 |

`/proc/[pid]/stat` parsing splits on the **first** `(` and the **last** `)`. A process named `a) b (c)` is otherwise unreadable.

Windows processes that refuse `OpenProcess` stay in the table with `readable = 0`. The renderer prints `n/a` for CPU and memory. Toolhelp still gave us a name and a thread count.

w64devkit's `tlhelp32.h` has `PROCESSENTRY32` / `Process32First` / `Process32Next` and no `PROCESSENTRY32A` alias. `collect_win.c` `#undef`s `UNICODE` before the headers so `szExeFile` stays `char *` and those unsuffixed names are the ANSI ones.

We store at most 768 rows. `process_count` is the kernel's figure; `truncated` is set if we had to stop. A busy Mac is typically 400–600; a Pi is far under the cap.

### Signalling

`collect_signal(pid, 15)` is `SIGTERM` on Unix and `TerminateProcess` on Windows. `collect_signal(pid, 9)` is `SIGKILL` / the same terminate. Unix refuses `pid <= 1`. The UI always confirms (`y` / `n` / Esc) before calling it.

---

## 4. The live loop vs `--once`

Two kinds of state matter:

- **Collector statics** — previous tick counters, keyed by pid+start. Survive across snapshots inside one process. Lost on exit.
- **Render options** — ascii / colour / paused / sort / filter / selected pid / scroll. Never written by the collector.

### When it starts and stops

| Hook | Meaning |
|------|--------|
| `collect_init` | Zero previous-tick buffers |
| first `collect_snapshot` | Identity + levels + process list; rates invalid |
| sleep + second snapshot | First honest frame |
| `qsort` | Hottest CPU first (unless `--sort` / `c/m/p/n`) |
| `term_init` | Alt screen (live only) |
| `term_poll_key` | Sleep that can be interrupted by `q` / arrows / `k` |
| `term_restore` / `atexit` | Cooked input, cursor on, previous scrollback |

### Update vs draw (important split)

| Function | Mutates | Draws |
|----------|---------|-------|
| `collect_snapshot` | snapshot + previous-tick statics | no |
| `qsort` / `sort_procs` | order of `procs[]` | no |
| `render_dashboard` | nothing in the snapshot | yes |
| `term_poll_key` | nothing | no |
| space / `c` / `/` / arrows | flags in `main` | no (next paint shows it) |

A tester debugging "CPU is always 0" should breakpoint the second `collect_snapshot` and watch `g_primed`. A tester debugging "the window is blank" should breakpoint `render_dashboard` / `fb_flush`. A tester debugging "cmd flashes every second" should confirm the live path is `FrameBuf` + one `fwrite`. A tester debugging "the CPU/memory header vanishes in a window" should watch `opt.cols` and `hline` / `main_bar_width` — a wrap adds rows and the top scrolls off. A tester debugging "q does nothing" should breakpoint `term_poll_key` and check that `ICANON` is off. A tester debugging "my shell is broken after Ctrl+C" should breakpoint `term_restore`. A tester debugging "Linux double-click does nothing" should breakpoint `linux_spawn_in_terminal`. A tester debugging "arrows do nothing" should breakpoint `decode_csi` / `decode_win_special`. A tester debugging "I killed the wrong pid" should breakpoint `collect_signal` and read `opt.selected_pid`. A tester debugging "ld: undefined collect_init on Windows" should check `UNAME_S` / `OS` in the Makefile, not `taskman.c`.

### Why not `sleep(1)` then `scanf`?

A blocking `fgets` would make the table freeze until Enter. A blocking `sleep` would ignore `q` until the interval ended. `poll` with a timeout is both the timer and the keyboard.

---

## Quick map of files

```
src/
  taskman.c           # main, flags, Linux no-TTY re-exec, prime, loop, sort, kill confirm
  snapshot.h          # TaskSnapshot
  collect.h           # host API
  collect_darwin.c    # Mach / libproc
  collect_linux.c     # /proc
  collect_win.c       # Toolhelp (PROCESSENTRY32) / GetProcessTimes
  posix_features.h    # first include on Linux (glibc C99)
  render.c            # bars, table, FrameBuf, filter match, wrap-safe header
  term.c              # alt screen, raw, poll, arrows, real window size
  util.c              # sleep, KiB labels, NO_COLOR, icase filter
  taskmantest.c       # make test
```

If you are tracing in a debugger, put breakpoints on `main`, `collect_snapshot`, `fill_procs` (in the host file), `sort_procs`, `render_dashboard`, and `term_restore`. You will see: **prime → sleep → delta → sort → paint → poll**.

See also `EXECUTION_FLOW.md` for a numbered walk of the first frame on macOS.
