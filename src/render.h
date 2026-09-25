#ifndef RENDER_H
#define RENDER_H

#include "snapshot.h"

#define SORT_CPU  0
#define SORT_MEM  1
#define SORT_PID  2
#define SORT_NAME 3

#define FILTER_CAP 48
#define BANNER_CAP 96

typedef struct {
    int ascii;          /* '#' / '.' instead of block glyphs */
    int color;          /* ANSI colour; honour NO_COLOR in taskman.c */
    int paused;
    double interval_sec;
    int cols;           /* terminal width, already clamped */
    int rows;           /* terminal height, already clamped */
    int sort_key;
    int sort_rev;
    char filter[FILTER_CAP];
    int filter_edit;    /* 1 while the user is typing a filter */
    int32_t selected_pid; /* -1 = none */
    int scroll;         /* first visible row in the filtered list */
    int confirm_kill;
    int kill_force;     /* 1 = SIGKILL / TerminateProcess "force" */
    int32_t self_pid;
    char banner[BANNER_CAP];
} RenderOptions;

void render_dashboard(const TaskSnapshot *snap, const RenderOptions *opt);
void render_once(const TaskSnapshot *snap, const RenderOptions *opt);

int  render_proc_matches(const ProcSample *p, const char *filter);
int  render_filtered_count(const TaskSnapshot *snap, const char *filter);
int  render_filtered_pos(const TaskSnapshot *snap, const char *filter,
                         int32_t pid);
int  render_page_rows(int rows);

#endif /* RENDER_H */
