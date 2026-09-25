#include "collect.h"
#include "util.h"

/* WORKING: w64devkit's tlhelp32.h typedefs PROCESSENTRY32 (ANSI) but
 * has no PROCESSENTRY32A alias. If UNICODE is on, the unsuffixed
 * name is wide and szExeFile is WCHAR. Force the A world so one
 * struct and Process32First/Next match util_copy_trunc. */
#undef UNICODE
#undef _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif

typedef struct {
    uint64_t idle;
    uint64_t total;
} CpuTicks;

typedef struct {
    int32_t  pid;
    uint64_t cpu_100ns;
    uint64_t start_time;
} PrevProc;

static CpuTicks g_cpu_all;
static PrevProc g_prev[SNAP_MAX_PROCS];
static int      g_prev_n;
static int      g_primed;
static double   g_last_t;

const char *collect_host_name(void)
{
    return "Windows";
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
    HANDLE h;
    BOOL ok;

    (void)sig;
    /* WORKING: Windows has no SIGTERM. Both polite and force map onto
     * TerminateProcess. pid 0 is the idle process — refuse it. */
    if (pid <= 0) {
        return -1;
    }
    h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (h == NULL) {
        return -1;
    }
    ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok ? 0 : -1;
}

static uint64_t filetime_u64(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return (uint64_t)u.QuadPart;
}

static uint64_t lookup_prev(int32_t pid, uint64_t start, int *found)
{
    int i;
    for (i = 0; i < g_prev_n; i++) {
        if (g_prev[i].pid == pid && g_prev[i].start_time == start) {
            *found = 1;
            return g_prev[i].cpu_100ns;
        }
    }
    *found = 0;
    return 0;
}

static void fill_identity(TaskSnapshot *out)
{
    DWORD n = (DWORD)(sizeof(out->hostname) - 1);
    SYSTEM_INFO si;

    GetComputerNameA(out->hostname, &n);
    util_copy_trunc(out->os_name, sizeof(out->os_name), "Windows");
    util_copy_trunc(out->host_label, sizeof(out->host_label), "Windows");
#ifdef _WIN64
    util_copy_trunc(out->arch, sizeof(out->arch), "x86_64");
#else
    util_copy_trunc(out->arch, sizeof(out->arch), "x86");
#endif
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
        util_copy_trunc(out->arch, sizeof(out->arch), "arm64");
    } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        util_copy_trunc(out->arch, sizeof(out->arch), "x86_64");
    }
    {
        OSVERSIONINFOA v;
        memset(&v, 0, sizeof(v));
        v.dwOSVersionInfoSize = sizeof(v);
        /* WORKING: GetVersionEx is the compatibility-shimmed API; it
         * may report 6.2 on newer Windows unless the manifest claims
         * otherwise. The dashboard only needs a hint, not a SKU. */
        if (GetVersionExA(&v)) {
            char rel[64];
            snprintf(rel, sizeof(rel), "%u.%u.%u",
                     (unsigned)v.dwMajorVersion,
                     (unsigned)v.dwMinorVersion,
                     (unsigned)v.dwBuildNumber);
            util_copy_trunc(out->os_release, sizeof(out->os_release), rel);
        }
    }
}

static void fill_cpu(TaskSnapshot *out)
{
    SYSTEM_INFO si;
    FILETIME idle_ft, kernel_ft, user_ft;
    uint64_t idle, kernel, user, total;

    GetSystemInfo(&si);
    out->cpu_count = (int)si.dwNumberOfProcessors;

    /* GetSystemTimes: KernelTime includes IdleTime. */
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        idle = filetime_u64(&idle_ft);
        kernel = filetime_u64(&kernel_ft);
        user = filetime_u64(&user_ft);
        total = kernel + user;
        if (g_primed && total > g_cpu_all.total) {
            uint64_t d_total = total - g_cpu_all.total;
            uint64_t d_idle = idle >= g_cpu_all.idle ? idle - g_cpu_all.idle : 0;
            if (d_idle > d_total) {
                d_idle = d_total;
            }
            out->cpu_total = util_clamp_pct(
                100.0 * (double)(d_total - d_idle) / (double)d_total);
        } else {
            out->cpu_total = -1.0;
        }
        g_cpu_all.idle = idle;
        g_cpu_all.total = total;
    } else {
        out->cpu_total = -1.0;
    }
}

static void fill_memory(TaskSnapshot *out)
{
    MEMORYSTATUSEX ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        out->mem_total = (uint64_t)ms.ullTotalPhys;
        out->mem_available = (uint64_t)ms.ullAvailPhys;
        if (out->mem_total > out->mem_available) {
            out->mem_used = out->mem_total - out->mem_available;
        }
    }
}

static void fill_user(HANDLE h, char *dst, size_t n)
{
    HANDLE tok = NULL;
    DWORD len = 0;
    TOKEN_USER *tu = NULL;
    char name[64];
    char domain[64];
    DWORD nlen = (DWORD)sizeof(name);
    DWORD dlen = (DWORD)sizeof(domain);
    SID_NAME_USE use;

    util_copy_trunc(dst, n, "-");
    if (h == NULL) {
        return;
    }
    if (!OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        return;
    }
    GetTokenInformation(tok, TokenUser, NULL, 0, &len);
    if (len == 0) {
        CloseHandle(tok);
        return;
    }
    tu = (TOKEN_USER *)malloc(len);
    if (tu != NULL &&
        GetTokenInformation(tok, TokenUser, tu, len, &len) &&
        LookupAccountSidA(NULL, tu->User.Sid, name, &nlen,
                          domain, &dlen, &use)) {
        util_copy_trunc(dst, n, name);
    }
    free(tu);
    CloseHandle(tok);
}

static void fill_procs(TaskSnapshot *out, double dt)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    int stored = 0;
    uint32_t seen = 0;
    PrevProc next[SNAP_MAX_PROCS];
    int next_n = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (!Process32First(snap, &pe)) {
        CloseHandle(snap);
        return;
    }

    do {
        ProcSample *p;
        HANDLE h;
        FILETIME ctime, etime, ktime, utime;
        PROCESS_MEMORY_COUNTERS pmc;
        uint64_t cpu_100ns = 0;
        uint64_t start = 0;
        int found = 0;
        uint64_t prev;
        char image[MAX_PATH];
        DWORD image_n;

        seen++;
        if (stored >= SNAP_MAX_PROCS) {
            out->truncated = 1;
            continue;
        }

        p = &out->procs[stored];
        memset(p, 0, sizeof(*p));
        p->pid = (int32_t)pe.th32ProcessID;
        p->ppid = (int32_t)pe.th32ParentProcessID;
        p->threads = (uint32_t)pe.cntThreads;
        util_copy_trunc(p->name, sizeof(p->name), pe.szExeFile);
        util_copy_trunc(p->command, sizeof(p->command), pe.szExeFile);
        util_copy_trunc(p->state, sizeof(p->state), "R");
        util_copy_trunc(p->user, sizeof(p->user), "-");

        /* WORKING: Protected / system processes refuse OpenProcess
         * without elevation. We still list them (Toolhelp gave us a
         * name and a thread count) but CPU and RSS stay at n/a. */
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                        pe.th32ProcessID);
        if (h == NULL) {
            h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                            FALSE, pe.th32ProcessID);
        }
        if (h != NULL) {
            p->readable = 1;
            if (GetProcessTimes(h, &ctime, &etime, &ktime, &utime)) {
                cpu_100ns = filetime_u64(&ktime) + filetime_u64(&utime);
                start = filetime_u64(&ctime);
            }
            memset(&pmc, 0, sizeof(pmc));
            if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
                p->rss_bytes = (uint64_t)pmc.WorkingSetSize;
                p->vsz_bytes = (uint64_t)pmc.PagefileUsage;
            }
            image_n = (DWORD)sizeof(image);
            if (QueryFullProcessImageNameA(h, 0, image, &image_n)) {
                util_copy_trunc(p->command, sizeof(p->command), image);
            }
            fill_user(h, p->user, sizeof(p->user));
            CloseHandle(h);
        } else {
            p->readable = 0;
            if (pe.th32ProcessID == 0) {
                util_copy_trunc(p->state, sizeof(p->state), "I");
            }
        }
        p->start_time = start;

        prev = lookup_prev(p->pid, start, &found);
        if (p->readable && g_primed && found && dt > 0.0 && cpu_100ns >= prev) {
            /* FILETIME is 100-ns ticks. 1e7 of them is one second. */
            double dsec = (double)(cpu_100ns - prev) / 10000000.0;
            p->cpu_pct = 100.0 * dsec / dt;
            if (p->cpu_pct < 0.0) {
                p->cpu_pct = 0.0;
            }
        } else if (!p->readable) {
            p->cpu_pct = g_primed ? 0.0 : -1.0;
        } else {
            p->cpu_pct = g_primed ? 0.0 : -1.0;
        }

        if (next_n < SNAP_MAX_PROCS) {
            next[next_n].pid = p->pid;
            next[next_n].cpu_100ns = cpu_100ns;
            next[next_n].start_time = start;
            next_n++;
        }
        stored++;
    } while (Process32Next(snap, &pe));

    CloseHandle(snap);
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
