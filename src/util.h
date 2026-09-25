#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>

void     util_sleep_ms(int ms);
double   util_now_sec(void);
int      util_clamp_int(int v, int lo, int hi);
double   util_clamp_pct(double v);

/* 1024-based, labelled KB/MB/GB so the numbers match Activity Monitor /
 * Task Manager more closely than SI (1000-based) units. */
void     util_format_bytes(uint64_t bytes, char *buf, size_t n);

int      util_nocolor_requested(void);

/* Cap-aware copy that does not go through printf (MinGW -Wformat-truncation). */
void     util_copy_trunc(char *dst, size_t n, const char *src);

/* Case-insensitive substring. Empty needle matches everything. */
int      util_str_has_icase(const char *hay, const char *needle);

#endif /* UTIL_H */
