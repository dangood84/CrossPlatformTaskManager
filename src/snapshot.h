#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stdint.h>

/* WORKING: one portable struct is the whole contract between a host
 * collector and the renderer. Darwin, Linux, and Windows fill the same
 * fields; nothing in render.c includes a Mach or Win32 header. */

#define SNAP_MAX_PROCS  768
#define SNAP_NAME_LEN    48
#define SNAP_CMD_LEN    128
#define SNAP_USER_LEN    24
#define SNAP_STATE_LEN    4
#define SNAP_HOST_LEN   256
#define SNAP_OS_LEN      64

typedef struct {
    int32_t  pid;
    int32_t  ppid;
    char     name[SNAP_NAME_LEN];
    char     command[SNAP_CMD_LEN];
    char     user[SNAP_USER_LEN];
    char     state[SNAP_STATE_LEN]; /* "R" / "S" / "Z" / ... */
    double   cpu_pct;               /* 100% = one busy core; -1 if not primed */
    uint64_t rss_bytes;
    uint64_t vsz_bytes;
    uint32_t threads;
    uint64_t start_time;            /* host units; used to detect PID reuse */
    int      readable;              /* 0 if the host could not open the task */
} ProcSample;

typedef struct {
    char     hostname[SNAP_HOST_LEN];
    char     os_name[SNAP_OS_LEN];
    char     os_release[SNAP_OS_LEN];
    char     arch[SNAP_OS_LEN];
    char     host_label[SNAP_OS_LEN]; /* "macOS" / "Linux" / "Windows" */

    int      cpu_count;        /* logical processors */
    double   cpu_total;        /* 0..100 of *all* cores; -1 if not primed */
    uint64_t mem_total;
    uint64_t mem_used;
    uint64_t mem_available;

    uint32_t process_count;    /* processes the kernel reported */
    int      proc_n;           /* how many we stored in procs[] */
    int      truncated;        /* 1 if process_count > SNAP_MAX_PROCS */
    ProcSample procs[SNAP_MAX_PROCS];

    double   sample_dt;        /* seconds between the two counter reads */
    int      primed;           /* 1 once CPU rates are meaningful */
} TaskSnapshot;

#endif /* SNAPSHOT_H */
