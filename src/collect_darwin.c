#include "collect.h"
#include "util.h"

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <pwd.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* WORKING: CPU rates are deltas. The first collect_snapshot only stores
 * tick / per-process CPU-time counters; the second (and later) turns
 * those into percentages. taskman.c always primes once. */
typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTicks;

typedef struct {
    int32_t  pid;
    uint64_t cpu_ns;
    uint64_t start_time;
} PrevProc;

static CpuTicks g_cpu_all;
static PrevProc g_prev[SNAP_MAX_PROCS];
static int      g_prev_n;
static int      g_primed;
static double   g_last_t;

const char *collect_host_name(void)
{
    return "macOS";
}

int collect_init(void)
{
    memset(&g_cpu_all, 0, sizeof(g_cpu_all));
    memset(g_prev, 0, sizeof(g_prev));
    g_prev_n = 0;
    g_primed = 0;
    g_last_t = 0.0;
    return 0;
}

void collect_shutdown(void)
{
}

int collect_signal(int32_t pid, int sig)
{
    /* Refuse idle / init. The kernel would ignore 0 anyway; 1 is launchd. */
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
        /* WORKING: PIDs wrap. If the start time changed, this is a new
         * process sitting in a recycled slot — do not subtract the
         * previous tenant's CPU time or we flash a multi-thousand % spike. */
        if (g_prev[i].pid == pid && g_prev[i].start_time == start) {
            *found = 1;
            return g_prev[i].cpu_ns;
        }
    }
    *found = 0;
    return 0;
}

static void fill_identity(TaskSnapshot *out)
{
    char ostype[64] = {0};
    char osrel[64] = {0};
    char machine[64] = {0};
    size_t n;

    memset(out->hostname, 0, sizeof(out->hostname));
    gethostname(out->hostname, sizeof(out->hostname) - 1);

    n = sizeof(ostype);
    sysctlbyname("kern.ostype", ostype, &n, NULL, 0);
    n = sizeof(osrel);
    sysctlbyname("kern.osrelease", osrel, &n, NULL, 0);
    n = sizeof(machine);
    sysctlbyname("hw.machine", machine, &n, NULL, 0);

    util_copy_trunc(out->os_name, sizeof(out->os_name),
                    ostype[0] ? ostype : "Darwin");
    util_copy_trunc(out->os_release, sizeof(out->os_release), osrel);
    util_copy_trunc(out->arch, sizeof(out->arch), machine);
    util_copy_trunc(out->host_label, sizeof(out->host_label), "macOS");
}

static void fill_cpu(TaskSnapshot *out)
{
    natural_t count = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t info_count = 0;
    processor_cpu_load_info_t cpu;
    kern_return_t kr;
    natural_t i;
    uint64_t all_idle = 0;
    uint64_t all_total = 0;

    kr = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                             &count, &info, &info_count);
    if (kr != KERN_SUCCESS || info == NULL || count == 0) {
        out->cpu_count = 0;
        out->cpu_total = -1.0;
        return;
    }

    cpu = (processor_cpu_load_info_t)info;
    out->cpu_count = (int)count;

    for (i = 0; i < count; i++) {
        uint64_t user = cpu[i].cpu_ticks[CPU_STATE_USER];
        uint64_t sys  = cpu[i].cpu_ticks[CPU_STATE_SYSTEM];
        uint64_t nice = cpu[i].cpu_ticks[CPU_STATE_NICE];
        uint64_t idle = cpu[i].cpu_ticks[CPU_STATE_IDLE];
        all_idle += idle;
        all_total += user + sys + nice + idle;
    }

    if (g_primed && all_total > g_cpu_all.total) {
        uint64_t d_total = all_total - g_cpu_all.total;
        uint64_t d_idle = all_idle - g_cpu_all.idle;
        out->cpu_total = util_clamp_pct(
            100.0 * (double)(d_total - d_idle) / (double)d_total);
    } else {
        out->cpu_total = -1.0;
    }
    g_cpu_all.idle = all_idle;
    g_cpu_all.total = all_total;

    vm_deallocate(mach_task_self(), (vm_address_t)info,
                  info_count * sizeof(integer_t));
}

static void fill_memory(TaskSnapshot *out)
{
    int64_t memsize = 0;
    size_t n = sizeof(memsize);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    uint64_t used;

    sysctlbyname("hw.memsize", &memsize, &n, NULL, 0);
    out->mem_total = (uint64_t)memsize;

    host_page_size(mach_host_self(), &page);
    memset(&vm, 0, sizeof(vm));
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        return;
    }

    /* WORKING: Activity Monitor's "Memory Used" is roughly
     *   (internal - purgeable) + wired + compressed
     * not "total - free". Free pages on macOS are a small leftover;
     * inactive/file-backed pages are reclaimable and should not count
     * as pressure the way they do on a naive Linux MemFree read. */
    used = ((uint64_t)vm.internal_page_count - (uint64_t)vm.purgeable_count
            + (uint64_t)vm.wire_count
            + (uint64_t)vm.compressor_page_count) * (uint64_t)page;
    if (used > out->mem_total) {
        used = out->mem_total;
    }
    out->mem_used = used;
    out->mem_available = out->mem_total - used;
}

static const char *state_letter(uint32_t st)
{
    switch (st) {
    case SIDL:   return "I";
    case SRUN:   return "R";
    case SSLEEP: return "S";
    case SSTOP:  return "T";
    case SZOMB:  return "Z";
    default:     return "?";
    }
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

static void fill_procs(TaskSnapshot *out, double dt)
{
    int mib[4];
    size_t size = 0;
    struct kinfo_proc *kp = NULL;
    int n;
    int i;
    int stored = 0;
    PrevProc next[SNAP_MAX_PROCS];
    int next_n = 0;

    /* WORKING: proc_listpids + PROC_PIDTBSDINFO skips a surprising
     * number of live pids (empty table slots, and BSD-info failures
     * on short-lived helpers). KERN_PROC_ALL is the same table
     * Activity Monitor starts from; we still use proc_pidinfo only
     * for CPU-nanoseconds and RSS. */
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_ALL;
    mib[3] = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) < 0 || size == 0) {
        return;
    }
    size += 16 * sizeof(*kp);
    kp = (struct kinfo_proc *)malloc(size);
    if (kp == NULL) {
        return;
    }
    if (sysctl(mib, 4, kp, &size, NULL, 0) < 0) {
        free(kp);
        return;
    }
    n = (int)(size / sizeof(*kp));

    for (i = 0; i < n; i++) {
        struct proc_taskinfo task;
        ProcSample *p;
        uint64_t cpu_ns = 0;
        uint64_t start;
        int found = 0;
        uint64_t prev_ns;
        char path[PROC_PIDPATHINFO_MAXSIZE];
        pid_t pid = kp[i].kp_proc.p_pid;

        if (pid <= 0) {
            continue;
        }
        out->process_count++;
        if (stored >= SNAP_MAX_PROCS) {
            out->truncated = 1;
            continue;
        }

        p = &out->procs[stored];
        memset(p, 0, sizeof(*p));
        p->pid = (int32_t)pid;
        p->ppid = (int32_t)kp[i].kp_eproc.e_ppid;
        p->readable = 1;
        start = (uint64_t)kp[i].kp_proc.p_starttime.tv_sec;
        p->start_time = start;
        util_copy_trunc(p->name, sizeof(p->name), kp[i].kp_proc.p_comm);
        util_copy_trunc(p->state, sizeof(p->state),
                        state_letter((uint32_t)kp[i].kp_proc.p_stat));
        fill_user(kp[i].kp_eproc.e_ucred.cr_uid, p->user, sizeof(p->user));
        {
            struct proc_bsdinfo bsd;
            memset(&bsd, 0, sizeof(bsd));
            if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd)) > 0
                && bsd.pbi_name[0] != '\0') {
                util_copy_trunc(p->name, sizeof(p->name), bsd.pbi_name);
            }
        }

        memset(&task, 0, sizeof(task));
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) > 0) {
            /* WORKING: pti_total_user / pti_total_system are nanoseconds
             * of CPU the process has used in its lifetime. 100% on the
             * table is one fully busy core (Activity Monitor / top). */
            cpu_ns = task.pti_total_user + task.pti_total_system;
            p->rss_bytes = task.pti_resident_size;
            p->vsz_bytes = task.pti_virtual_size;
            p->threads = (uint32_t)task.pti_threadnum;
        } else if (kp[i].kp_proc.p_stat == SZOMB) {
            p->readable = 0;
        }

        if (proc_pidpath(pid, path, sizeof(path)) > 0) {
            util_copy_trunc(p->command, sizeof(p->command), path);
        } else {
            util_copy_trunc(p->command, sizeof(p->command), p->name);
        }

        prev_ns = lookup_prev(p->pid, start, &found);
        if (g_primed && found && dt > 0.0 && cpu_ns >= prev_ns) {
            p->cpu_pct = 100.0 * ((double)(cpu_ns - prev_ns) / 1e9) / dt;
            if (p->cpu_pct < 0.0) {
                p->cpu_pct = 0.0;
            }
        } else {
            p->cpu_pct = g_primed ? 0.0 : -1.0;
        }

        if (next_n < SNAP_MAX_PROCS) {
            next[next_n].pid = p->pid;
            next[next_n].cpu_ns = cpu_ns;
            next[next_n].start_time = start;
            next_n++;
        }
        stored++;
    }

    free(kp);
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
