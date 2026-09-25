# Execution flow: from `main` to a drawn process row

A step-by-step trace of what happens from `int main` through host initialisation, down to how two Mach (or `/proc`, or Win32) counter reads become `CPU  16.8` on a selected row.

Default launch (`make run`) opens the **live macOS table**. `make linux` / `make windows` use the same `taskman.c` and `render.c`; only the collect step changes. This trace is **macOS** (`collect_darwin.c`) unless a step says otherwise.

One thread does everything:

- **main** — flags, prime, sleep, collect, sort, paint, poll

There is no GUI thread and no worker. `collect_snapshot` and `fwrite` of the composed frame run on the same stack that called `main`.

---

## Phase A — process entry

**1.** The OS loads `build/task-manager`. C runtime initialises. Static collector buffers (`g_cpu_all`, `g_prev[]`, …) are zero.

**2.** `main` walks `argv`. No flags means: live mode, 1.0 s interval, sort by CPU descending, Unicode bars, colour unless `NO_COLOR` is set.

On **Linux**, if stdin, stdout, and stderr are all non-TTYs (file-manager click, `xdg-open`, macOS-style `open`) and `--once` was not passed, `linux_spawn_in_terminal` forks a terminal emulator and this process exits. The child has `TASK_MANAGER_IN_TERM=1` and starts again at step 1 with a real TTY. That check runs **before** the prime sleep so a desktop click is not delayed by one interval.

```c
/* src/taskman.c */
if (collect_init() != 0) ...
snap = calloc(1, sizeof(*snap));
collect_snapshot(snap);                  /* step 5 */
util_sleep_ms((int)(interval * 1000.0)); /* step 6 */
collect_snapshot(snap);                  /* step 10 */
sort_procs(snap);
```

**3.** `signal(SIGINT)` / `SIGTERM` point at `on_stop`, which only sets `g_stop`. The handler does **not** call `term_restore` — `term_init` has not run yet, and doing TTY work in a handler is unsafe. `atexit` will restore if we got as far as `term_init`.

**4.** `collect_init` (Darwin) zeroes the previous-tick structs and sets `g_primed = 0`.

Windows / Linux: the same zeroing. Files and Toolhelp snapshots are opened later, per snapshot.

---

## Phase B — prime (first snapshot, rates invalid)

**5.** `collect_snapshot` `memset`s the out-struct, then:

| Call | What it reads | What the dashboard would show |
|------|----------------|-------------------------------|
| `fill_identity` | `gethostname`, `kern.ostype`, `kern.osrelease`, `hw.machine` | `Daniels-MacBook-Pro-2.local  ·  macOS 23.6.0  ·  arm64` |
| `fill_cpu` | `host_processor_info` tick counters | `cpu_total = -1`, `primed = 0` |
| `fill_memory` | `hw.memsize` + `HOST_VM_INFO64` | used / total, already honest |
| `fill_procs` | `sysctl(KERN_PROC_ALL)` + `PROC_PIDTASKINFO` + `proc_pidpath` | every row `cpu_pct = -1`; names, RSS, users already honest |

**6.** The function stores this tick's system idle/total and each process's CPU-nanoseconds + start time in static `g_*` buffers, sets `g_primed = 1` for **next** time, and returns. The snapshot the caller holds still has `primed == 0` (the value *before* that assignment).

**7.** `util_sleep_ms(1000)`. `clock_gettime(CLOCK_MONOTONIC)` is not involved in the sleep itself; `nanosleep` is. If `SIGWINCH` arrives, the remainder is slept. Testers: do not expect the prime to be shorter than the interval unless the process is killed.

Windows prime uses `Sleep`. Linux is the same `nanosleep`.

---

## Phase C — second snapshot (first honest frame)

**8.** `collect_snapshot` again. `dt = now - g_last_t` (about 1.00 s).

**9.** `fill_cpu` reads Mach ticks a second time:

```
d_total = this_total - g_cpu_all.total
d_idle  = this_idle  - g_cpu_all.idle
usage   = 100 * (d_total - d_idle) / d_total
```

That is the headline bar (0..100 of all cores).

**10.** `fill_procs` walks `KERN_PROC_ALL` again. For each pid whose `(pid, p_starttime)` matches a previous row:

```
delta_ns = this_cpu_ns - prev_cpu_ns
cpu_pct  = 100 * (delta_ns / 1e9) / dt
```

A process that appeared since the last tick has no previous match → `cpu_pct = 0` this frame, real rate on the next. A recycled pid with a *new* start time is treated the same way (no spike).

**11.** Memory is a level, not a rate. It is just read again. It was already valid on the first snapshot.

**12.** The snapshot now has `primed = 1`, `sample_dt ≈ 1.0`, `cpu_total` in `0..100`, and honest per-row `cpu_pct`.

Linux at this step is `/proc/stat` plus `utime+stime` ticks. Windows is `GetSystemTimes` plus `GetProcessTimes`. Kernel time on Windows **includes** idle on the headline counters; the collector subtracts it. Per-process kernel time does not include the idle process.

---

## Phase D — sort, then paint (`--once` or first live frame)

**13.** `sort_procs` `qsort`s `snap->procs[0..proc_n)`. Default comparator: CPU descending, then RSS descending, then PID ascending. `--sort mem|pid|name` and the live `c/m/p/n` / `r` keys change `g_sort_key` / `g_sort_rev` and sort again.

**14.** `--once`, `make once`, or a non-TTY **stdout** (pipe / `less`): `render_once` → `paint` into a `FrameBuf` → one `fwrite` → process exits. Terminal mode is unchanged. A fully detached Linux GUI launch never reaches here; it was re-exec'd in step 2. This is what `make test` is *not* — the test binary never calls `render_*` at all; it only asserts ranges and prints one `ok` line.

**15.** Live TTY: `term_init`:

```
tcgetattr → clear ICANON, ECHO; keep ISIG
\x1b[?1049h     enter alternate screen
\x1b[?25l       hide cursor
atexit(term_restore)
```

Windows: `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, UTF-8 `SetConsoleOutputCP`, console input without line-echo.

**16.** Loop body:

```
term_size → opt.cols / opt.rows     /* TIOCGWINSZ, clamped */
if (!paused && !filter_edit && !confirm_kill) collect + sort
clamp_view                          /* keep selected pid on screen */
render_dashboard                    /* FrameBuf: \x1b[H + paint + \x1b[K/line + one fwrite + \x1b[J */
key = term_poll_key(interval or 200ms)
```

**17.** `paint` writes, in order:

1. Title + optional `PAUSED`
2. Hostname · host label · release · arch · core count · process count
3. Headline CPU bar (`cpu_total`)
4. Memory bar (used / total)
5. Column headings
6. One row per visible filtered process (`▶` on the selected pid; cyan on our own pid)
7. Confirm-kill line, or filter-edit line, or a banner, or the key legend
8. Footer: sort / filter / `from-to of N`

Bar fill is `round(pct / 100 * width)`. Colour: green `< 60`, yellow `< 85`, red otherwise. `--ascii` uses `#` / `.` / `>`.

`paint` does **not** write to stdout as it goes. It appends to a `FrameBuf`. `render_dashboard` prefixes `\x1b[H`, suffixes `\x1b[J`, and `fwrite`s the lot. That is why cmd looks as still as bash: the console is unbuffered, but one write is one picture.

**18.** `term_poll_key`: `poll(stdin, interval_ms)`.

| Byte / sequence | Action |
|-----------------|--------|
| `q` / `Q` / `0x03` | break the loop |
| space | toggle `paused` |
| `c` / `m` / `p` / `n` | sort key |
| `r` | reverse sort |
| `/` | enter filter edit |
| `k` / `K` | confirm terminate / force-kill |
| arrows, `j`, PgUp/PgDn, Home/End | move selection |
| Esc (lone, 30 ms) | clear filter |
| CSI leftovers | drained, ignored |
| none (timeout) | next collect + frame |

While `filter_edit` or `confirm_kill` is set, collect is skipped and the poll timeout drops to 200 ms so typing feels immediate.

`ISIG` is still on, so Ctrl+C is usually a **signal**, not `0x03`. `on_stop` sets `g_stop`; the loop notices after `poll` returns (`EINTR` or timeout).

---

## The repeating loop

```text
main thread
  → collect_snapshot          (Mach / /proc / Win32)
        fill_* write TaskSnapshot
  → qsort                     (CPU / mem / pid / name)
  → render_dashboard
        FrameBuf: \x1b[H + bars + table + \x1b[K each line
        one fwrite, then \x1b[J
  → term_poll_key(interval)
        q → restore → exit
        space → paused := !paused
        arrows → selected_pid moves
        k → confirm → collect_signal
        timeout → next tick
```

Quit: `q` → `term_restore` (`\x1b[?25h\x1b[?1049l` + `tcsetattr` old) → `collect_shutdown` → `return 0`. The previous scrollback is back. Ctrl+C is the same path via `g_stop`.

Pause after step 16: further iterations skip `collect_snapshot`. The percentages freeze. The footer clock still updates because `paint` calls `time()` every frame.

---

## Keyboard path (no model.Press)

There is no key-to-enum table like the calculator. A handful of ASCII bytes plus `TERM_KEY_*` from CSI / Win32 specials matter. UTF-8 paste into the filter is ignored one continuation-byte at a time (only 32..126 are appended).

Resize: no `SIGWINCH` handler. The next frame calls `term_size` again. A tester who wants a 100-column layout should resize, wait one interval, and look at `opt.cols`. Under 100 columns `paint` uses the compact identity line and a shorter memory annotation so the CPU/memory bars stay on the first rows of a windowed Mac Terminal / lxterminal / cmd window. Hlines are `cols - 1` glyphs: a full-width rule plus newline wraps on macOS Terminal and the header scrolls away.

---

## Windows path (same draw loop)

`main` is identical. Differences that matter when tracing:

1. `collect_win.c` — Toolhelp (`PROCESSENTRY32` / `Process32First`, after `#undef UNICODE`) + `GetProcessTimes` / `GetProcessMemoryInfo` / `QueryFullProcessImageNameA` / `LookupAccountSidA` / `GetSystemTimes` / `GlobalMemoryStatusEx`. w64devkit has no `PROCESSENTRY32A`.
2. Processes that refuse `OpenProcess` have `readable = 0`; the row prints `n/a`.
3. `collect_signal` is `TerminateProcess` for both 15 and 9. pid 0 is refused.
4. `term_poll_key` is `_kbhit` + `Sleep(20)` in a `GetTickCount` window. Arrow keys arrive as `0`/`224` + a follow-up and become `TERM_KEY_UP` etc.
5. Names use `util_copy_trunc` (capped `memcpy`), not `snprintf("%s")`, so MinGW `-Wformat-truncation` stays quiet. Memory-label `snprintf` uses 24-byte byte-count buffers for the same reason.
6. Live paint is one `fwrite`. Do not expect a blank flash between ticks.
7. `make` on w64devkit keys off `uname` = `Windows` or `OS=Windows_NT` and writes `build/task-manager.exe`. `make windows` writes `build/TaskManager.exe`. Both link `collect_win.c`.

Close the console window: the process dies; `atexit` still runs `term_restore` if `term_init` ran.

---

## Linux path (same draw loop)

1. `/proc/stat` first line = headline CPU.
2. `/proc/meminfo` `MemAvailable` (or Free+Buffers+Cached).
3. Digit directories under `/proc`. `stat` is parsed by first `(` / last `)`. `status` supplies `Uid` and `VmRSS`. `cmdline` NULs become spaces.
4. Kernel threads (empty cmdline) keep the `stat` comm as the command.

`make` on the Pi (or `make linux`) links `collect_linux.c`. `make linux` on a Mac will compile that file and then fail at run time if `/proc` is missing. Run Linux targets **on Linux**.

`posix_features.h` (first include) plus `-D_POSIX_C_SOURCE=200809L` is why `sigaction` / `nanosleep` / `gethostname` / `kill` exist under `-std=c99`. Without it, the Pi compile stops with “storage size of `sa` isn’t known”.

Desktop launch with no TTY is step 2, not a failed paint.

---

## One-line map

`main` → (Linux: maybe re-exec in a terminal) → `collect_init` → **snapshot (store counters)** → **sleep** → **snapshot (subtract)** → **sort** → `term_init` → **collect → sort → FrameBuf + fwrite → poll** → `term_restore`.

Debugger: `main`, `collect_snapshot`, `fill_procs`, `sort_procs`, `render_dashboard`, `term_poll_key`, `collect_signal`, `term_restore`. The first *visible* live frame is already the second sample; there is no "warming up" line unless you breakpoint before the prime sleep and force a paint.

See also `WORKINGS.md` for who owns which field and the Activity Monitor / `MemAvailable` / one-core CPU definitions in more detail.
