#include "posix_features.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

void util_sleep_ms(int ms)
{
    if (ms < 0) {
        ms = 0;
    }
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    /* WORKING: nanosleep is restartable; a SIGWINCH mid-sleep should not
     * skip the rest of the interval or the dashboard stutters. */
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0) {
        /* EINTR: req now holds the remainder. Anything else: give up. */
        if (req.tv_sec <= 0 && req.tv_nsec <= 0) {
            break;
        }
    }
#endif
}

double util_now_sec(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int got_freq = 0;
    LARGE_INTEGER now;
    if (!got_freq) {
        QueryPerformanceFrequency(&freq);
        got_freq = 1;
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

int util_clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

double util_clamp_pct(double v)
{
    if (v < 0.0) {
        return 0.0;
    }
    if (v > 100.0) {
        return 100.0;
    }
    return v;
}

static void format_scaled(double value, const char *const units[], int nunits,
                          char *buf, size_t n)
{
    int u = 0;
    while (u < nunits - 1 && value >= 1024.0) {
        value /= 1024.0;
        u++;
    }
    if (u == 0) {
        snprintf(buf, n, "%.0f %s", value, units[u]);
    } else if (value >= 10.0) {
        snprintf(buf, n, "%.1f %s", value, units[u]);
    } else {
        snprintf(buf, n, "%.2f %s", value, units[u]);
    }
}

void util_format_bytes(uint64_t bytes, char *buf, size_t n)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    format_scaled((double)bytes, units, 6, buf, n);
}

int util_nocolor_requested(void)
{
    /* https://no-color.org/ — any non-empty value means "please don't". */
    const char *e = getenv("NO_COLOR");
    return e != NULL && e[0] != '\0';
}

void util_copy_trunc(char *dst, size_t n, const char *src)
{
    /* WORKING: MinGW's -Wformat-truncation fires on snprintf("%s") when
     * the source is a slice of a bigger buffer (Toolhelp exe names,
     * /proc comm). This copy is explicit about the cap. */
    if (dst == NULL || n == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    {
        size_t len = strlen(src);
        if (len >= n) {
            len = n - 1;
        }
        memcpy(dst, src, len);
        dst[len] = '\0';
    }
}

int util_str_has_icase(const char *hay, const char *needle)
{
    size_t nlen;
    size_t i;

    if (needle == NULL || needle[0] == '\0') {
        return 1;
    }
    if (hay == NULL) {
        return 0;
    }
    nlen = strlen(needle);
    for (i = 0; hay[i] != '\0'; i++) {
        size_t j = 0;
        while (j < nlen) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (a == '\0') {
                return 0;
            }
            if (tolower(a) != tolower(b)) {
                break;
            }
            j++;
        }
        if (j == nlen) {
            return 1;
        }
    }
    return 0;
}
