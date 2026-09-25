#ifndef COLLECT_H
#define COLLECT_H

#include "snapshot.h"

/* Each OS compiles exactly one collect_*.c, the same idea as the Resource
 * Monitor and the Pascal projects' {$IFDEF} host units. The function
 * names stay identical so taskman.c never mentions Darwin or Win32. */

int         collect_init(void);
void        collect_shutdown(void);
int         collect_snapshot(TaskSnapshot *out);
const char *collect_host_name(void);

/* Send a signal / terminate. sig 15 = polite (SIGTERM), sig 9 = force
 * (SIGKILL). Windows maps both onto TerminateProcess. Returns 0 on
 * success. pid <= 1 is refused on Unix so we cannot signal init. */
int         collect_signal(int32_t pid, int sig);

#endif /* COLLECT_H */
