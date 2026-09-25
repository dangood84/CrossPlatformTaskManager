#include "posix_features.h"
#include "collect.h"
#include "render.h"
#include "term.h"
#include "util.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <limits.h>
#endif

/* WORKING: SIGINT/SIGTERM must not leave the terminal in raw mode or
 * the user's shell looks broken after Ctrl+C. atexit in term_init also
 * restores; the flag lets the loop exit cleanly instead of _Exit. */
static volatile sig_atomic_t g_stop = 0;

static int g_sort_key = SORT_CPU;
static int g_sort_rev = 0;

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
        "Cross-Platform Task Manager\n"
        "\n"
        "Usage: task-manager [options]\n"
        "\n"
        "  -1, --once          print one snapshot and exit (no raw terminal)\n"
        "  -n, --interval SEC  refresh interval (default 1.0, min 0.2, max 60)\n"
        "      --sort KEY      cpu (default), mem, pid, or name\n"
        "      --filter SUB    case-insensitive name/command/user match\n"
        "      --ascii         '#' / '.' bars instead of Unicode blocks\n"
        "      --no-color      disable ANSI colours (also honours NO_COLOR)\n"
        "  -h, --help          this text\n"
        "\n"
        "Keys (live mode):\n"
        "  q quit     space pause     c/m/p/n sort     r reverse\n"
        "  / filter   k terminate     K force-kill     arrows / PgUp / PgDn\n"
        "\n"
        "See README.md, WORKINGS.md, and EXECUTION_FLOW.md.\n");
}

#ifdef __linux__
/* WORKING: `open` is a macOS command. On the Pi, xdg-open / a file-manager
 * double-click starts this binary with no controlling terminal. The live
 * dashboard needs a TTY, so without help it primed, printed one frame to
 * nowhere, and exited — "nothing happens".
 *
 * If stdin/stdout/stderr are all non-TTYs and the user did not pass
 * --once, open a real terminal emulator and re-exec ourselves. The env
 * var stops a fork bomb if the child is still detached. */
static int linux_spawn_in_terminal(const char *argv0)
{
    char self[PATH_MAX];
    const char *path = argv0;
    ssize_t n;
    pid_t pid;

    if (getenv("TASK_MANAGER_IN_TERM") != NULL) {
        return -1;
    }

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        path = self;
    }
    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        setenv("TASK_MANAGER_IN_TERM", "1", 1);
        execlp("x-terminal-emulator", "x-terminal-emulator", "-e", path, (char *)NULL);
        execlp("lxterminal", "lxterminal", "-e", path, (char *)NULL);
        execlp("xterm", "xterm", "-e", path, (char *)NULL);
        _exit(127);
    }
    return 0;
}
#endif

static double parse_interval(const char *s)
{
    double v = atof(s);
    if (v < 0.2) {
        v = 0.2;
    }
    if (v > 60.0) {
        v = 60.0;
    }
    return v;
}

static int parse_sort(const char *s)
{
    if (s == NULL) {
        return SORT_CPU;
    }
    if (strcmp(s, "mem") == 0 || strcmp(s, "memory") == 0) {
        return SORT_MEM;
    }
    if (strcmp(s, "pid") == 0) {
        return SORT_PID;
    }
    if (strcmp(s, "name") == 0 || strcmp(s, "cmd") == 0) {
        return SORT_NAME;
    }
    return SORT_CPU;
}

static int name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp_procs(const void *va, const void *vb)
{
    const ProcSample *a = (const ProcSample *)va;
    const ProcSample *b = (const ProcSample *)vb;
    int c = 0;

    switch (g_sort_key) {
    case SORT_MEM:
        if (a->rss_bytes < b->rss_bytes) {
            c = -1;
        } else if (a->rss_bytes > b->rss_bytes) {
            c = 1;
        }
        break;
    case SORT_PID:
        if (a->pid < b->pid) {
            c = -1;
        } else if (a->pid > b->pid) {
            c = 1;
        }
        break;
    case SORT_NAME:
        c = name_cmp(a->name, b->name);
        break;
    case SORT_CPU:
    default:
        if (a->cpu_pct < b->cpu_pct) {
            c = -1;
        } else if (a->cpu_pct > b->cpu_pct) {
            c = 1;
        }
        break;
    }

    if (c == 0) {
        if (a->rss_bytes < b->rss_bytes) {
            c = -1;
        } else if (a->rss_bytes > b->rss_bytes) {
            c = 1;
        } else if (a->pid < b->pid) {
            c = -1;
        } else if (a->pid > b->pid) {
            c = 1;
        }
    }

    /* WORKING: CPU and memory default to hottest / fattest first — that
     * is what you open a task manager for. PID and name stay A→Z / 0→N
     * until the user hits r. */
    if (g_sort_key == SORT_CPU || g_sort_key == SORT_MEM) {
        c = -c;
    }
    if (g_sort_rev) {
        c = -c;
    }
    return c;
}

static void sort_procs(TaskSnapshot *snap)
{
    if (snap->proc_n > 1) {
        qsort(snap->procs, (size_t)snap->proc_n, sizeof(ProcSample), cmp_procs);
    }
}

static int32_t pid_at(const TaskSnapshot *snap, const char *filter, int index)
{
    int pos = 0;
    int i;
    for (i = 0; i < snap->proc_n; i++) {
        if (!render_proc_matches(&snap->procs[i], filter)) {
            continue;
        }
        if (pos == index) {
            return snap->procs[i].pid;
        }
        pos++;
    }
    return -1;
}

static void clamp_view(TaskSnapshot *snap, RenderOptions *opt)
{
    int view_n = render_filtered_count(snap, opt->filter);
    int page = render_page_rows(opt->rows);
    int pos;

    if (view_n <= 0) {
        opt->selected_pid = -1;
        opt->scroll = 0;
        return;
    }
    pos = render_filtered_pos(snap, opt->filter, opt->selected_pid);
    if (pos < 0) {
        opt->selected_pid = pid_at(snap, opt->filter, 0);
        pos = 0;
    }
    if (pos < opt->scroll) {
        opt->scroll = pos;
    }
    if (pos >= opt->scroll + page) {
        opt->scroll = pos - page + 1;
    }
    if (opt->scroll < 0) {
        opt->scroll = 0;
    }
    if (opt->scroll > view_n - 1) {
        opt->scroll = view_n - 1;
    }
}

static void move_sel(TaskSnapshot *snap, RenderOptions *opt, int delta)
{
    int view_n = render_filtered_count(snap, opt->filter);
    int pos;
    if (view_n <= 0) {
        return;
    }
    pos = render_filtered_pos(snap, opt->filter, opt->selected_pid);
    if (pos < 0) {
        pos = 0;
    }
    pos += delta;
    if (pos < 0) {
        pos = 0;
    }
    if (pos > view_n - 1) {
        pos = view_n - 1;
    }
    opt->selected_pid = pid_at(snap, opt->filter, pos);
}

#ifdef _WIN32
static int32_t self_pid(void)
{
    return (int32_t)GetCurrentProcessId();
}
#else
static int32_t self_pid(void)
{
    return (int32_t)getpid();
}
#endif

static void filter_add(RenderOptions *opt, int ch)
{
    size_t n = strlen(opt->filter);
    if (n + 1 >= FILTER_CAP) {
        return;
    }
    if (ch < 32 || ch > 126) {
        return;
    }
    opt->filter[n] = (char)ch;
    opt->filter[n + 1] = '\0';
}

static void filter_backspace(RenderOptions *opt)
{
    size_t n = strlen(opt->filter);
    if (n > 0) {
        opt->filter[n - 1] = '\0';
    }
}

int main(int argc, char **argv)
{
    int once = 0;
    int ascii = 0;
    int color = 1;
    double interval = 1.0;
    int i;
    TaskSnapshot *snap;
    RenderOptions opt;
    int paused = 0;
    char initial_filter[FILTER_CAP];

    initial_filter[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--ascii") == 0) {
            ascii = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            color = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if ((strcmp(argv[i], "-n") == 0 ||
                    strcmp(argv[i], "--interval") == 0) && i + 1 < argc) {
            interval = parse_interval(argv[++i]);
        } else if (strncmp(argv[i], "--interval=", 11) == 0) {
            interval = parse_interval(argv[i] + 11);
        } else if ((strcmp(argv[i], "--sort") == 0) && i + 1 < argc) {
            g_sort_key = parse_sort(argv[++i]);
        } else if (strncmp(argv[i], "--sort=", 7) == 0) {
            g_sort_key = parse_sort(argv[i] + 7);
        } else if ((strcmp(argv[i], "--filter") == 0) && i + 1 < argc) {
            util_copy_trunc(initial_filter, sizeof(initial_filter), argv[++i]);
        } else if (strncmp(argv[i], "--filter=", 9) == 0) {
            util_copy_trunc(initial_filter, sizeof(initial_filter), argv[i] + 9);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (util_nocolor_requested()) {
        color = 0;
    }

#ifdef __linux__
    if (!once &&
        !isatty(STDIN_FILENO) &&
        !isatty(STDOUT_FILENO) &&
        !isatty(STDERR_FILENO)) {
        if (linux_spawn_in_terminal(argc > 0 ? argv[0] : NULL) == 0) {
            return 0;
        }
        fprintf(stderr,
                "task-manager: this is a terminal dashboard.\n"
                "Run it from a terminal, not with 'open' or a silent GUI click:\n"
                "  ./build/task-manager\n"
                "  ./build/task-manager --once\n");
        return 1;
    }
#endif

    if (collect_init() != 0) {
        fprintf(stderr, "collect_init failed on %s\n", collect_host_name());
        return 1;
    }

#ifdef _WIN32
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_stop;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        /* WORKING: SIGPIPE is irrelevant (we only write stdout) but
         * SIGWINCH is ignored: the next frame re-reads TIOCGWINSZ. */
        signal(SIGPIPE, SIG_IGN);
    }
#endif

    /* WORKING: the snapshot holds up to SNAP_MAX_PROCS rows (~180 KB).
     * Heap, not stack — Raspberry Pi OS can give a smaller default
     * stack than macOS, and a stack smash here is a mysterious crash
     * on first collect. */
    snap = (TaskSnapshot *)calloc(1, sizeof(*snap));
    if (snap == NULL) {
        fprintf(stderr, "out of memory\n");
        collect_shutdown();
        return 1;
    }

    /* Prime: first snapshot stores counters, second computes rates. */
    if (collect_snapshot(snap) != 0) {
        fprintf(stderr, "collect_snapshot failed\n");
        free(snap);
        collect_shutdown();
        return 1;
    }
    util_sleep_ms((int)(interval * 1000.0));
    if (collect_snapshot(snap) != 0) {
        fprintf(stderr, "collect_snapshot failed\n");
        free(snap);
        collect_shutdown();
        return 1;
    }
    sort_procs(snap);

    memset(&opt, 0, sizeof(opt));
    opt.ascii = ascii;
    opt.color = color;
    opt.interval_sec = interval;
    opt.cols = 80;
    opt.rows = 24;
    if (term_is_tty()) {
        int cols = 80, rows = 24;
        term_size(&cols, &rows);
        opt.cols = cols;
        opt.rows = rows;
    }
    opt.sort_key = g_sort_key;
    opt.sort_rev = g_sort_rev;
    opt.selected_pid = -1;
    opt.self_pid = self_pid();
    util_copy_trunc(opt.filter, sizeof(opt.filter), initial_filter);
    clamp_view(snap, &opt);

    if (once) {
        render_once(snap, &opt);
        free(snap);
        collect_shutdown();
        return 0;
    }

    if (!term_is_tty()) {
        /* WORKING: piped to a file or `less`, a live alt-screen loop is
         * the wrong tool. Print one framed snapshot the way --once does.
         * A fully detached GUI launch is handled earlier on Linux. */
        fprintf(stderr,
                "task-manager: no TTY on stdout; printing one snapshot.\n"
                "For the live dashboard run this in a terminal:\n"
                "  ./build/task-manager\n");
        render_once(snap, &opt);
        free(snap);
        collect_shutdown();
        return 0;
    }

    term_init();

    while (!g_stop) {
        int key;
        int cols = 80, rows = 24;
        int poll_ms;

        term_size(&cols, &rows);
        opt.cols = cols;
        opt.rows = rows;
        opt.paused = paused;
        opt.sort_key = g_sort_key;
        opt.sort_rev = g_sort_rev;

        if (!paused && !opt.filter_edit && !opt.confirm_kill) {
            if (collect_snapshot(snap) != 0) {
                break;
            }
            sort_procs(snap);
        }
        clamp_view(snap, &opt);
        render_dashboard(snap, &opt);
        if (!opt.filter_edit && !opt.confirm_kill) {
            opt.banner[0] = '\0';
        }

        /* Filter typing should feel immediate; otherwise the poll
         * timeout *is* the refresh interval. */
        poll_ms = (opt.filter_edit || opt.confirm_kill)
                      ? 200
                      : (int)(interval * 1000.0);
        key = term_poll_key(poll_ms);
        if (g_stop) {
            break;
        }

        if (opt.confirm_kill) {
            if (key == 'y' || key == 'Y') {
                int sig = opt.kill_force ? 9 : 15;
                int32_t pid = opt.selected_pid;
                if (collect_signal(pid, sig) == 0) {
                    snprintf(opt.banner, sizeof(opt.banner),
                             "signalled PID %d", (int)pid);
                } else {
                    snprintf(opt.banner, sizeof(opt.banner),
                             "could not signal PID %d (permission?)", (int)pid);
                }
                opt.confirm_kill = 0;
                opt.kill_force = 0;
            } else if (key == 'n' || key == 'N' || key == TERM_KEY_ESC ||
                       key == 'q' || key == 'Q') {
                opt.confirm_kill = 0;
                opt.kill_force = 0;
            }
            continue;
        }

        if (opt.filter_edit) {
            if (key == TERM_KEY_ENTER) {
                opt.filter_edit = 0;
            } else if (key == TERM_KEY_ESC) {
                opt.filter[0] = '\0';
                opt.filter_edit = 0;
            } else if (key == TERM_KEY_BACKSPACE) {
                filter_backspace(&opt);
            } else if (key > 0 && key < 256) {
                filter_add(&opt, key);
            }
            continue;
        }

        if (key == 'q' || key == 'Q' || key == 3) {
            break;
        }
        if (key == ' ') {
            paused = !paused;
        } else if (key == 'c' || key == 'C') {
            g_sort_key = SORT_CPU;
        } else if (key == 'm' || key == 'M') {
            g_sort_key = SORT_MEM;
        } else if (key == 'p' || key == 'P') {
            g_sort_key = SORT_PID;
        } else if (key == 'n' || key == 'N') {
            g_sort_key = SORT_NAME;
        } else if (key == 'r' || key == 'R') {
            g_sort_rev = !g_sort_rev;
        } else if (key == '/') {
            opt.filter_edit = 1;
        } else if (key == TERM_KEY_ESC) {
            opt.filter[0] = '\0';
        } else if (key == 'k') {
            if (opt.selected_pid > 1 ||
                (opt.selected_pid > 0 &&
                 strcmp(collect_host_name(), "Windows") == 0)) {
                opt.confirm_kill = 1;
                opt.kill_force = 0;
            }
        } else if (key == 'K') {
            if (opt.selected_pid > 1 ||
                (opt.selected_pid > 0 &&
                 strcmp(collect_host_name(), "Windows") == 0)) {
                opt.confirm_kill = 1;
                opt.kill_force = 1;
            }
        } else if (key == TERM_KEY_UP) {
            move_sel(snap, &opt, -1);
        } else if (key == TERM_KEY_DOWN || key == 'j') {
            move_sel(snap, &opt, 1);
        } else if (key == TERM_KEY_PGUP) {
            move_sel(snap, &opt, -render_page_rows(opt.rows));
        } else if (key == TERM_KEY_PGDN) {
            move_sel(snap, &opt, render_page_rows(opt.rows));
        } else if (key == TERM_KEY_HOME) {
            opt.selected_pid = pid_at(snap, opt.filter, 0);
        } else if (key == TERM_KEY_END) {
            {
                int vn = render_filtered_count(snap, opt.filter);
                if (vn > 0) {
                    opt.selected_pid = pid_at(snap, opt.filter, vn - 1);
                }
            }
        }

        if (key == 'c' || key == 'C' || key == 'm' || key == 'M' ||
            key == 'p' || key == 'P' || key == 'n' || key == 'N' ||
            key == 'r' || key == 'R') {
            sort_procs(snap);
        }
    }

    term_restore();
    free(snap);
    collect_shutdown();
    return 0;
}
