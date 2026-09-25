#include "posix_features.h"
#include "render.h"
#include "term.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *C_RESET = "\x1b[0m";
static const char *C_BOLD  = "\x1b[1m";
static const char *C_DIM   = "\x1b[2m";
static const char *C_GREEN = "\x1b[32m";
static const char *C_YELL  = "\x1b[33m";
static const char *C_RED   = "\x1b[31m";
static const char *C_CYAN  = "\x1b[36m";
static const char *C_REV   = "\x1b[7m";

/* WORKING: cmd.exe (and the MinGW CRT) treat a console as unbuffered, so
 * each fprintf("...\n") hits the glass immediately. Combined with a
 * leading \x1b[J (erase display) you see: blank flash, then the table
 * painted top-to-bottom, every second. Unix TTYs keep the writes in a
 * stdio buffer until fflush, which is why the Pi and the Mac look still.
 *
 * Fix: build the whole frame in memory and fwrite it once. Live mode
 * homes the cursor and clears each line with \x1b[K instead of wiping
 * the screen first. */
#define FRAME_CAP 32768

typedef struct {
    char   data[FRAME_CAP];
    size_t len;
    int    live; /* 1: \x1b[K before newline so leftover glyphs vanish */
} FrameBuf;

static void fb_add(FrameBuf *fb, const char *s)
{
    size_t n;
    size_t room;

    if (s == NULL || fb->len + 1 >= FRAME_CAP) {
        return;
    }
    n = strlen(s);
    room = FRAME_CAP - 1 - fb->len;
    if (n > room) {
        n = room;
    }
    memcpy(fb->data + fb->len, s, n);
    fb->len += n;
    fb->data[fb->len] = '\0';
}

static void fb_addch(FrameBuf *fb, int c)
{
    if (fb->len + 1 >= FRAME_CAP) {
        return;
    }
    fb->data[fb->len++] = (char)c;
    fb->data[fb->len] = '\0';
}

static void fb_printf(FrameBuf *fb, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t room = FRAME_CAP - 1 - fb->len;

    if (room == 0) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(fb->data + fb->len, room + 1, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n > room) {
            fb->len += room;
        } else {
            fb->len += (size_t)n;
        }
    }
}

static void fb_nl(FrameBuf *fb)
{
    /* Erase-to-end-of-line, then newline. Overwrite-in-place, no flash. */
    if (fb->live) {
        fb_add(fb, "\x1b[K");
    }
    fb_addch(fb, '\n');
}

static const char *pct_color(double pct, int color)
{
    if (!color) {
        return "";
    }
    if (pct >= 85.0) {
        return C_RED;
    }
    if (pct >= 60.0) {
        return C_YELL;
    }
    return C_GREEN;
}

static const char *s(const RenderOptions *opt, const char *code)
{
    return opt->color ? code : "";
}

static void bar(FrameBuf *fb, const RenderOptions *opt, double pct, int width)
{
    const char *on;
    const char *off;
    const char *col;
    int filled;
    int i;

    if (width < 4) {
        width = 4;
    }
    pct = util_clamp_pct(pct);
    filled = (int)(pct / 100.0 * (double)width + 0.5);
    if (filled > width) {
        filled = width;
    }
    /* WORKING: Unicode block + light shade read as a meter at a glance.
     * --ascii is for consoles that still mangle UTF-8 (old cmd.exe). */
    on = opt->ascii ? "#" : "\xE2\x96\x88";   /* █ */
    off = opt->ascii ? "." : "\xE2\x96\x91";  /* ░ */
    col = pct_color(pct, opt->color);

    fb_add(fb, col);
    fb_addch(fb, '[');
    for (i = 0; i < filled; i++) {
        fb_add(fb, on);
    }
    for (i = filled; i < width; i++) {
        fb_add(fb, off);
    }
    fb_addch(fb, ']');
    fb_add(fb, s(opt, C_RESET));
}

static double used_pct(uint64_t used, uint64_t total)
{
    if (total == 0) {
        return 0.0;
    }
    return util_clamp_pct(100.0 * (double)used / (double)total);
}

static void hline(FrameBuf *fb, const RenderOptions *opt, int cols)
{
    int i;
    const char *ch = opt->ascii ? "-" : "\xE2\x94\x80"; /* ─ */
    fb_add(fb, s(opt, C_DIM));
    for (i = 0; i < cols; i++) {
        fb_add(fb, ch);
    }
    fb_add(fb, s(opt, C_RESET));
    fb_nl(fb);
}

static int main_bar_width(int cols)
{
    int w = cols - 36;
    return util_clamp_int(w, 16, 48);
}

int render_proc_matches(const ProcSample *p, const char *filter)
{
    if (p == NULL) {
        return 0;
    }
    if (filter == NULL || filter[0] == '\0') {
        return 1;
    }
    return util_str_has_icase(p->name, filter) ||
           util_str_has_icase(p->command, filter) ||
           util_str_has_icase(p->user, filter);
}

int render_filtered_count(const TaskSnapshot *snap, const char *filter)
{
    int n = 0;
    int i;
    if (snap == NULL) {
        return 0;
    }
    for (i = 0; i < snap->proc_n; i++) {
        if (render_proc_matches(&snap->procs[i], filter)) {
            n++;
        }
    }
    return n;
}

int render_filtered_pos(const TaskSnapshot *snap, const char *filter,
                        int32_t pid)
{
    int pos = 0;
    int i;
    if (snap == NULL || pid < 0) {
        return -1;
    }
    for (i = 0; i < snap->proc_n; i++) {
        if (!render_proc_matches(&snap->procs[i], filter)) {
            continue;
        }
        if (snap->procs[i].pid == pid) {
            return pos;
        }
        pos++;
    }
    return -1;
}

int render_page_rows(int rows)
{
    /* title, host, hline, cpu, mem, hline, colnames, table..., hline, footer */
    return util_clamp_int(rows - 10, 3, 60);
}

static const char *sort_label(int key)
{
    if (key == SORT_MEM) {
        return "MEM";
    }
    if (key == SORT_PID) {
        return "PID";
    }
    if (key == SORT_NAME) {
        return "NAME";
    }
    return "CPU";
}

static void paint_proc(FrameBuf *fb, const RenderOptions *opt,
                       const TaskSnapshot *snap, const ProcSample *p,
                       int selected, int name_w)
{
    char rss[32];
    double mpct;
    const char *col;
    const char *arrow;
    char name[SNAP_CMD_LEN];
    size_t nlen;

    util_format_bytes(p->rss_bytes, rss, sizeof(rss));
    mpct = used_pct(p->rss_bytes, snap->mem_total);

    if (opt->ascii) {
        arrow = selected ? ">" : " ";
    } else {
        arrow = selected ? "\xE2\x96\xB6" : " "; /* ▶ */
    }

    if (p->command[0] != '\0' && name_w >= 40) {
        util_copy_trunc(name, sizeof(name), p->command);
    } else {
        util_copy_trunc(name, sizeof(name),
                        p->name[0] ? p->name : p->command);
    }
    nlen = strlen(name);
    if ((int)nlen > name_w && name_w > 0) {
        name[name_w] = '\0';
    }

    if (selected) {
        fb_add(fb, s(opt, C_REV));
    } else if (p->pid == opt->self_pid) {
        fb_add(fb, s(opt, C_CYAN));
    }

    fb_printf(fb, " %s %6d %-12.12s ", arrow, (int)p->pid,
              p->user[0] ? p->user : "-");

    if (!p->readable) {
        fb_add(fb, selected ? "" : s(opt, C_DIM));
        fb_printf(fb, "   n/a    n/a %9s %4s  %-2s  %s",
                  "-", "-",
                  p->state[0] ? p->state : "-", name);
    } else if (p->cpu_pct < 0.0) {
        fb_printf(fb, "     — %5.1f%% %9s %4u  %-2s  %s",
                  mpct, rss, (unsigned)p->threads,
                  p->state[0] ? p->state : "-", name);
    } else {
        /* Per-process CPU can exceed 100% (one-core scale). Colour the
         * number by a clamped copy so a 250% compile job still reads red. */
        col = pct_color(p->cpu_pct > 100.0 ? 100.0 : p->cpu_pct,
                        opt->color && !selected);
        fb_add(fb, col);
        fb_printf(fb, "%6.1f", p->cpu_pct);
        if (selected) {
            fb_add(fb, s(opt, C_REV));
        } else {
            fb_add(fb, s(opt, C_RESET));
            if (p->pid == opt->self_pid) {
                fb_add(fb, s(opt, C_CYAN));
            }
        }
        fb_printf(fb, " %5.1f%% %9s %4u  %-2s  %s",
                  mpct, rss, (unsigned)p->threads,
                  p->state[0] ? p->state : "-", name);
    }

    fb_add(fb, s(opt, C_RESET));
    fb_nl(fb);
}

static void paint(FrameBuf *fb, const TaskSnapshot *snap, const RenderOptions *opt)
{
    char b1[64], b2[64], when[64];
    time_t now;
    struct tm *tm;
    int cols = opt->cols;
    int bw = main_bar_width(cols);
    int page = render_page_rows(opt->rows);
    int view_n;
    int name_w;
    int i;
    int shown = 0;
    double mpct;
    now = time(NULL);
    tm = localtime(&now);
    if (tm != NULL) {
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        snprintf(when, sizeof(when), "--");
    }

    view_n = render_filtered_count(snap, opt->filter);

    /* PID(7)+user(12)+cpu(6)+mem(6)+rss(9)+thr(4)+st(2)+pads ≈ 56 */
    name_w = util_clamp_int(cols - 56, 8, SNAP_CMD_LEN - 1);

    fb_add(fb, s(opt, C_BOLD));
    fb_add(fb, "  Cross-Platform Task Manager");
    fb_add(fb, s(opt, C_RESET));
    if (opt->paused) {
        fb_add(fb, s(opt, C_YELL));
        fb_add(fb, "   PAUSED");
        fb_add(fb, s(opt, C_RESET));
    }
    fb_nl(fb);

    fb_printf(fb, "  %s%s%s  ·  %s %s  ·  %s  ·  %d cores  ·  %u processes",
              s(opt, C_CYAN),
              snap->hostname[0] ? snap->hostname : "unknown",
              s(opt, C_RESET),
              snap->host_label[0] ? snap->host_label : snap->os_name,
              snap->os_release,
              snap->arch,
              snap->cpu_count,
              (unsigned)snap->process_count);
    if (snap->truncated) {
        fb_printf(fb, "  %s(showing %d)%s",
                  s(opt, C_DIM), snap->proc_n, s(opt, C_RESET));
    }
    fb_nl(fb);
    hline(fb, opt, cols);

    fb_add(fb, "  CPU     ");
    if (!snap->primed || snap->cpu_total < 0.0) {
        fb_add(fb, s(opt, C_DIM));
        fb_add(fb, "(warming up — rates need two samples)");
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    } else {
        bar(fb, opt, snap->cpu_total, bw);
        fb_printf(fb, "  %5.1f%%", snap->cpu_total);
        fb_nl(fb);
    }

    mpct = used_pct(snap->mem_used, snap->mem_total);
    util_format_bytes(snap->mem_used, b1, sizeof(b1));
    util_format_bytes(snap->mem_total, b2, sizeof(b2));
    fb_add(fb, "  Memory  ");
    bar(fb, opt, mpct, bw);
    fb_printf(fb, "  %s / %s  (%4.1f%%)", b1, b2, mpct);
    fb_nl(fb);
    hline(fb, opt, cols);

    fb_add(fb, s(opt, C_DIM));
    fb_printf(fb, "    %6s %-12s %6s %6s %9s %4s  %-2s  %s",
              "PID", "USER", "CPU%", "MEM%", "RSS", "THR", "ST", "NAME");
    fb_add(fb, s(opt, C_RESET));
    fb_nl(fb);

    {
        int pos = 0;
        for (i = 0; i < snap->proc_n && shown < page; i++) {
            int selected;
            if (!render_proc_matches(&snap->procs[i], opt->filter)) {
                continue;
            }
            if (pos < opt->scroll) {
                pos++;
                continue;
            }
            selected = (snap->procs[i].pid == opt->selected_pid);
            paint_proc(fb, opt, snap, &snap->procs[i], selected, name_w);
            shown++;
            pos++;
        }
    }

    if (shown == 0) {
        fb_add(fb, s(opt, C_DIM));
        if (snap->proc_n == 0) {
            fb_add(fb, "  (no processes reported)");
        } else {
            fb_add(fb, "  (no processes match the filter)");
        }
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    }

    while (shown < page) {
        fb_nl(fb);
        shown++;
    }

    hline(fb, opt, cols);

    if (opt->confirm_kill && opt->selected_pid >= 0) {
        fb_add(fb, s(opt, C_YELL));
        fb_printf(fb, "  %s PID %d ?   y yes   n / esc cancel",
                  opt->kill_force ? "KILL" : "Terminate",
                  (int)opt->selected_pid);
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    } else if (opt->filter_edit) {
        fb_printf(fb, "  Filter: %s%s_%s    enter apply   esc clear",
                  s(opt, C_BOLD), opt->filter, s(opt, C_RESET));
        fb_nl(fb);
    } else if (opt->banner[0] != '\0') {
        fb_add(fb, s(opt, C_YELL));
        fb_printf(fb, "  %s", opt->banner);
        fb_add(fb, s(opt, C_RESET));
        fb_nl(fb);
    } else {
        fb_printf(fb,
                  "  %sq%s quit  %sspace%s pause  %sc/m/p/n%s sort  %sr%s rev  "
                  "%s/%s filter  %sk%s kill  %s↑↓%s   %.1fs  %s",
                  s(opt, C_BOLD), s(opt, C_RESET),
                  s(opt, C_BOLD), s(opt, C_RESET),
                  s(opt, C_BOLD), s(opt, C_RESET),
                  s(opt, C_BOLD), s(opt, C_RESET),
                  s(opt, C_BOLD), s(opt, C_RESET),
                  s(opt, C_BOLD), s(opt, C_RESET),
                  s(opt, C_BOLD), s(opt, C_RESET),
                  opt->interval_sec, when);
        fb_nl(fb);
    }

    {
        int from = view_n == 0 ? 0 : opt->scroll + 1;
        int to = opt->scroll + (shown < page && view_n > 0
                                ? (view_n - opt->scroll) : page);
        if (to > view_n) {
            to = view_n;
        }
        if (from > view_n) {
            from = view_n;
        }
        fb_printf(fb, "  sort=%s%s  filter=%s%s%s   %d-%d of %d",
                  sort_label(opt->sort_key),
                  opt->sort_rev ? " (rev)" : "",
                  s(opt, C_CYAN),
                  opt->filter[0] ? opt->filter : "-",
                  s(opt, C_RESET),
                  from, to, view_n);
        fb_nl(fb);
    }
}

static void fb_flush(const FrameBuf *fb)
{
    if (fb->len > 0) {
        fwrite(fb->data, 1, fb->len, stdout);
        fflush(stdout);
    }
}

void render_dashboard(const TaskSnapshot *snap, const RenderOptions *opt)
{
    FrameBuf fb;

    memset(&fb, 0, sizeof(fb));
    fb.live = 1;
    /* Home only — do not erase the display first (that is the cmd flash). */
    fb_add(&fb, "\x1b[H");
    paint(&fb, snap, opt);
    fb_add(&fb, "\x1b[J");
    fb_flush(&fb);
}

void render_once(const TaskSnapshot *snap, const RenderOptions *opt)
{
    FrameBuf fb;

    memset(&fb, 0, sizeof(fb));
    fb.live = 0;
    paint(&fb, snap, opt);
    fb_flush(&fb);
}
