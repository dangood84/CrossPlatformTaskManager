#include "posix_features.h"
#include "collect.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTicks;

typedef struct {
    int32_t  pid;
    uint64_t cpu_ticks;
    uint64_t start_time;
} PrevProc;

static CpuTicks g_cpu_all;
static PrevProc g_prev[SNAP_MAX_PROCS];
static int      g_prev_n;
static int      g_primed;
static double   g_last_t;
static long     g_clk_tck;
static long     g_page;

const char *collect_host_name(void)
{
    return "Linux";
}

int collect_init(void)
{
    memset(&g_cpu_all, 0, sizeof(g_cpu_all));
    memset(g_prev, 0, sizeof(g_prev));
    g_prev_n = 0;
    g_primed = 0;
    g_last_t = 0.0;
    g_clk_tck = sysconf(_SC_CLK_TCK);
    if (g_clk_tck <= 0) {
        g_clk_tck = 100;
    }
    g_page = sysconf(_SC_PAGESIZE);
    if (g_page <= 0) {
        g_page = 4096;
    }
    return 0;
}

void collect_shutdown(void)
{
}

int collect_signal(int32_t pid, int sig)
{
    if (pid <= 1) {
        errno = EPERM;
        return -1;
    }
    if (sig != SIGKILL) {
        sig = SIGTERM;
    }
    return kill((pid_t)pid, sig) == 0 ? 0 : -1;
}

static uint64_t lookup_prev(int32_t pid, uint64_t start, int *found)
{
    int i;
    for (i = 0; i < g_prev_n; i++) {
        if (g_prev[i].pid == pid && g_prev[i].start_time == start) {
            *found = 1;
            return g_prev[i].cpu_ticks;
        }
    }
    *found = 0;
    return 0;
}

static void fill_identity(TaskSnapshot *out)
{
    struct utsname u;
    gethostname(out->hostname, sizeof(out->hostname) - 1);
    if (uname(&u) == 0) {
        util_copy_trunc(out->os_name, sizeof(out->os_name), u.sysname);
        util_copy_trunc(out->os_release, sizeof(out->os_release), u.release);
        util_copy_trunc(out->arch, sizeof(out->arch), u.machine);
    }
    util_copy_trunc(out->host_label, sizeof(out->host_label), "Linux");
}

static void fill_cpu(TaskSnapshot *out)
{
    FILE *f;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    uint64_t idle_all, total;
    long ncpu;

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    out->cpu_count = ncpu > 0 ? (int)ncpu : 0;

    f = fopen("/proc/stat", "r");
    if (f == NULL) {
        out->cpu_total = -1.0;
        return;
    }

    /* WORKING: /proc/stat's first line is the aggregate of every cpuN
     * line. Idle is idle+iowait (time the CPU was not running a thread);
     * steal is included in total so a busy hypervisor still shows up. */
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle,
               &iowait, &irq, &softirq, &steal) >= 4) {
        idle_all = (uint64_t)idle + (uint64_t)iowait;
        total = (uint64_t)user + (uint64_t)nice + (uint64_t)system
              + (uint64_t)idle + (uint64_t)iowait + (uint64_t)irq
              + (uint64_t)softirq + (uint64_t)steal;
        if (g_primed && total > g_cpu_all.total) {
            uint64_t d_total = total - g_cpu_all.total;
            uint64_t d_idle = idle_all - g_cpu_all.idle;
            if (d_idle > d_total) {
                d_idle = d_total;
            }
            out->cpu_total = util_clamp_pct(
                100.0 * (double)(d_total - d_idle) / (double)d_total);
        } else {
            out->cpu_total = -1.0;
        }
        g_cpu_all.idle = idle_all;
        g_cpu_all.total = total;
    } else {
        out->cpu_total = -1.0;
    }
    fclose(f);
}

static void fill_memory(TaskSnapshot *out)
{
    FILE *f;
    char key[64];
    unsigned long long val = 0;
    char unit[32];
    uint64_t total = 0, avail = 0, free_k = 0, buffers = 0, cached = 0;
    int got_avail = 0;

    f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        return;
    }
    while (fscanf(f, "%63s %llu %31s", key, &val, unit) == 3) {
        uint64_t bytes = val * 1024ULL;
        if (strcmp(key, "MemTotal:") == 0) {
            total = bytes;
        } else if (strcmp(key, "MemAvailable:") == 0) {
            avail = bytes;
            got_avail = 1;
        } else if (strcmp(key, "MemFree:") == 0) {
            free_k = bytes;
        } else if (strcmp(key, "Buffers:") == 0) {
            buffers = bytes;
        } else if (strcmp(key, "Cached:") == 0) {
            cached = bytes;
        }
    }
    fclose(f);

    out->mem_total = total;
    if (!got_avail) {
        /* Pre-3.14 kernels have no MemAvailable. */
        avail = free_k + buffers + cached;
    }
    if (avail > total) {
        avail = total;
    }
    out->mem_available = avail;
    out->mem_used = total > avail ? total - avail : 0;
}

static int is_pid_dir(const char *name)
{
    int i;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    for (i = 0; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static void fill_user(uid_t uid, char *dst, size_t n)
{
    struct passwd *pw = getpwuid(uid);
    if (pw != NULL && pw->pw_name != NULL) {
        util_copy_trunc(dst, n, pw->pw_name);
    } else {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)uid);
        util_copy_trunc(dst, n, tmp);
    }
}

static int read_status_uid_rss(pid_t pid, uid_t *uid, uint64_t *rss_kb,
                               uint64_t *vsz_kb)
{
    char path[64];
    FILE *f;
    char line[256];
    int got_uid = 0;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "Uid:", 4) == 0) {
            unsigned u = 0;
            if (sscanf(line + 4, "%u", &u) == 1) {
                *uid = (uid_t)u;
                got_uid = 1;
            }
        } else if (strncmp(line, "VmRSS:", 6) == 0) {
            unsigned long long v = 0;
            sscanf(line + 6, "%llu", &v);
            *rss_kb = (uint64_t)v;
        } else if (strncmp(line, "VmSize:", 7) == 0) {
            unsigned long long v = 0;
            sscanf(line + 7, "%llu", &v);
            *vsz_kb = (uint64_t)v;
        }
    }
    fclose(f);
    return got_uid ? 0 : -1;
}

static void read_cmdline(pid_t pid, char *dst, size_t n)
{
    char path[64];
    FILE *f;
    size_t got;
    size_t i;

    dst[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    got = fread(dst, 1, n - 1, f);
    fclose(f);
    dst[got] = '\0';
    for (i = 0; i < got; i++) {
        if (dst[i] == '\0') {
            dst[i] = ' ';
        }
    }
    while (got > 0 && dst[got - 1] == ' ') {
        dst[--got] = '\0';
    }
}

static int parse_stat(const char *line, char *comm, size_t comm_n,
                      char *state, int *ppid, unsigned long *utime,
                      unsigned long *stime, long *threads,
                      unsigned long long *start, unsigned long *vsize,
                      long *rss)
{
    const char *lparen;
    const char *rparen;
    size_t n;

    /* WORKING: /proc/[pid]/stat is "pid (comm) state ppid ...". comm
     * itself may contain spaces and parentheses (a process named
     * "a) b (c)"), so we split on the *first* '(' and the *last* ')'. */
    lparen = strchr(line, '(');
    rparen = strrchr(line, ')');
    if (lparen == NULL || rparen == NULL || rparen <= lparen + 1) {
        return -1;
    }
    n = (size_t)(rparen - lparen - 1);
    if (n >= comm_n) {
        n = comm_n - 1;
    }
    memcpy(comm, lparen + 1, n);
    comm[n] = '\0';

    if (sscanf(rparen + 2,
               "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
               "%lu %lu %*d %*d %*d %*d %ld %*d %llu %lu %ld",
               state, ppid, utime, stime, threads, start, vsize, rss) < 8) {
        return -1;
    }
    return 0;
}

static void fill_procs(TaskSnapshot *out, double dt)
{
    DIR *dir;
    struct dirent *ent;
    int stored = 0;
    uint32_t seen = 0;
    PrevProc next[SNAP_MAX_PROCS];
    int next_n = 0;
    char statec;

    dir = opendir("/proc");
    if (dir == NULL) {
        return;
    }

    while ((ent = readdir(dir)) != NULL) {
        pid_t pid;
        char path[64];
        char line[1024];
        FILE *f;
        char comm[SNAP_NAME_LEN];
        int ppid = 0;
        unsigned long utime = 0, stime = 0, vsize = 0;
        long threads = 0, rss_pages = 0;
        unsigned long long start = 0;
        uid_t uid = 0;
        uint64_t rss_kb = 0, vsz_kb = 0;
        ProcSample *p;
        uint64_t cpu_ticks;
        int found = 0;
        uint64_t prev_ticks;

        if (!is_pid_dir(ent->d_name)) {
            continue;
        }
        pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) {
            continue;
        }
        seen++;

        snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
        f = fopen(path, "r");
        if (f == NULL) {
            continue;
        }
        if (fgets(line, sizeof(line), f) == NULL) {
            fclose(f);
            continue;
        }
        fclose(f);

        memset(comm, 0, sizeof(comm));
        statec = '?';
        if (parse_stat(line, comm, sizeof(comm), &statec, &ppid,
                       &utime, &stime, &threads, &start, &vsize,
                       &rss_pages) != 0) {
            continue;
        }

        if (stored >= SNAP_MAX_PROCS) {
            out->truncated = 1;
            continue;
        }

        p = &out->procs[stored];
        memset(p, 0, sizeof(*p));
        p->pid = (int32_t)pid;
        p->ppid = (int32_t)ppid;
        p->readable = 1;
        p->start_time = (uint64_t)start;
        p->threads = threads > 0 ? (uint32_t)threads : 0;
        p->state[0] = statec;
        p->state[1] = '\0';
        util_copy_trunc(p->name, sizeof(p->name), comm);

        if (read_status_uid_rss(pid, &uid, &rss_kb, &vsz_kb) == 0) {
            fill_user(uid, p->user, sizeof(p->user));
            p->rss_bytes = rss_kb * 1024ULL;
            p->vsz_bytes = vsz_kb * 1024ULL;
        } else {
            fill_user((uid_t)0, p->user, sizeof(p->user));
            p->rss_bytes = (uint64_t)rss_pages * (uint64_t)g_page;
            p->vsz_bytes = (uint64_t)vsize;
        }

        read_cmdline(pid, p->command, sizeof(p->command));
        if (p->command[0] == '\0') {
            /* Kernel threads have an empty cmdline; keep the comm. */
            util_copy_trunc(p->command, sizeof(p->command), p->name);
        }

        cpu_ticks = (uint64_t)utime + (uint64_t)stime;
        prev_ticks = lookup_prev(p->pid, p->start_time, &found);
        if (g_primed && found && dt > 0.0 && cpu_ticks >= prev_ticks) {
            double dsec = (double)(cpu_ticks - prev_ticks) / (double)g_clk_tck;
            p->cpu_pct = 100.0 * dsec / dt;
            if (p->cpu_pct < 0.0) {
                p->cpu_pct = 0.0;
            }
        } else {
            p->cpu_pct = g_primed ? 0.0 : -1.0;
        }

        if (next_n < SNAP_MAX_PROCS) {
            next[next_n].pid = p->pid;
            next[next_n].cpu_ticks = cpu_ticks;
            next[next_n].start_time = p->start_time;
            next_n++;
        }
        stored++;
    }
    closedir(dir);

    out->process_count = seen;
    out->proc_n = stored;
    memcpy(g_prev, next, (size_t)next_n * sizeof(PrevProc));
    g_prev_n = next_n;
}

int collect_snapshot(TaskSnapshot *out)
{
    double now;
    double dt;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->cpu_total = -1.0;

    now = util_now_sec();
    dt = (g_last_t > 0.0) ? (now - g_last_t) : 0.0;
    if (dt < 0.0) {
        dt = 0.0;
    }

    fill_identity(out);
    fill_cpu(out);
    fill_memory(out);
    fill_procs(out, dt);

    out->sample_dt = dt;
    out->primed = g_primed;
    g_primed = 1;
    g_last_t = now;
    return 0;
}
