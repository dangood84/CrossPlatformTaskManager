#include "posix_features.h"
#include "collect.h"
#include "util.h"

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

/* Headless checks — the same role as monitortest.c in the Resource
 * Monitor. No raw terminal, no dashboard. Two samples so CPU rates
 * are primed. */

static int fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
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

int main(void)
{
    TaskSnapshot *a;
    TaskSnapshot *b;
    int rc = 0;
    int i;
    int found_self = 0;
    int32_t me;

    if (collect_init() != 0) {
        return fail("collect_init");
    }

    a = (TaskSnapshot *)calloc(1, sizeof(*a));
    b = (TaskSnapshot *)calloc(1, sizeof(*b));
    if (a == NULL || b == NULL) {
        collect_shutdown();
        return fail("calloc snapshot");
    }

    if (collect_snapshot(a) != 0) {
        collect_shutdown();
        free(a);
        free(b);
        return fail("first collect_snapshot");
    }
    if (a->hostname[0] == '\0') {
        rc |= fail("hostname empty");
    }
    if (a->cpu_count <= 0) {
        rc |= fail("cpu_count <= 0");
    }
    if (a->mem_total == 0) {
        rc |= fail("mem_total is 0");
    }
    if (a->primed) {
        rc |= fail("first snapshot should not be primed");
    }
    if (a->process_count == 0 && a->proc_n == 0) {
        rc |= fail("no processes on first snapshot");
    }

    util_sleep_ms(250);
    if (collect_snapshot(b) != 0) {
        collect_shutdown();
        free(a);
        free(b);
        return fail("second collect_snapshot");
    }
    if (!b->primed) {
        rc |= fail("second snapshot should be primed");
    }
    if (b->cpu_total < 0.0 || b->cpu_total > 100.0) {
        rc |= fail("cpu_total out of range");
    }
    if (b->mem_used > b->mem_total) {
        rc |= fail("mem_used > mem_total");
    }
    if (b->sample_dt <= 0.0) {
        rc |= fail("sample_dt should be > 0 after two reads");
    }
    if (b->proc_n <= 0) {
        rc |= fail("proc_n <= 0");
    }

    me = self_pid();
    for (i = 0; i < b->proc_n; i++) {
        if (b->procs[i].name[0] == '\0' && b->procs[i].command[0] == '\0') {
            rc |= fail("process with empty name and command");
            break;
        }
        if (b->procs[i].pid == me) {
            found_self = 1;
        }
        if (b->procs[i].cpu_pct < -0.01) {
            rc |= fail("primed process still has cpu_pct < 0");
            break;
        }
    }
    if (!found_self) {
        rc |= fail("own pid missing from process list");
    }

    printf("ok  host=%s  hostname=%s  cpus=%d  cpu=%.1f%%  mem=%llu/%llu  "
           "procs=%u stored=%d  self=%d  primed=%d  dt=%.3fs\n",
           collect_host_name(),
           b->hostname,
           b->cpu_count,
           b->cpu_total,
           (unsigned long long)b->mem_used,
           (unsigned long long)b->mem_total,
           (unsigned)b->process_count,
           b->proc_n,
           (int)me,
           b->primed,
           b->sample_dt);

    collect_shutdown();
    free(a);
    free(b);
    return rc;
}
