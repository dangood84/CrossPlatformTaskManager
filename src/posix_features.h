#ifndef POSIX_FEATURES_H
#define POSIX_FEATURES_H

/* WORKING: gcc -std=c99 on glibc is *strict* ISO C. sigaction, nanosleep,
 * clock_gettime, gethostname, and kill live in POSIX headers and stay
 * hidden unless a feature-test macro is set before any system include.
 *
 * macOS and MinGW headers are laxer, which is why those hosts compiled
 * without this. Linux (including Raspberry Pi OS) did not.
 *
 * Only define these on Linux. _POSIX_C_SOURCE on Darwin can hide BSD
 * helpers (libproc, getpwuid in some SDK mixes) that collect_darwin.c
 * needs. */

#if defined(__linux__)
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#endif /* POSIX_FEATURES_H */
