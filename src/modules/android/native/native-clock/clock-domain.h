#pragma once
#ifndef __ASSEMBLER__
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include "clock-cpu.h"

/* Translation-unit-local adaptation, never a libSystem/JVM interposer.
 * Java nanoTime, CACurrentMediaTime and this uptime clock share Mach absolute
 * time. Continuous time is read independently; no measured offset is cached. */
static inline int klj_host_clock_gettime(clockid_t clock, struct timespec *out) {
    return clock_gettime(clock == CLOCK_MONOTONIC ? CLOCK_UPTIME_RAW : clock, out);
}
#define clock_gettime klj_host_clock_gettime

static inline int klj_clock_native(int clock) {
    switch (clock) {
    case 0: case 5: case 8: case 11: return CLOCK_REALTIME;
    case 1: case 4: case 6: return CLOCK_UPTIME_RAW;
    case 7: case 9: return CLOCK_MONOTONIC_RAW;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}
static inline int klj_clock_read(int clock, struct timespec *out) {
    if(clock<0)return klj_clock_cpu_read(clock,out,0);
    int native = klj_clock_native(clock);
    if (native < 0) { errno = EINVAL; return -1; }
    if (!out) { errno = EFAULT; return -1; }
    return clock_gettime((clockid_t)native, out);
}
static inline int klj_clock_resolution(int clock,struct timespec *out){
    if(clock<0)return klj_clock_cpu_read(clock,out,1);
    int native=klj_clock_native(clock);struct timespec discarded;
    if(native<0){errno=EINVAL;return -1;}
    return clock_getres((clockid_t)native,out?out:&discarded);
}
static inline int klj_nsec_valid(const struct timespec *value) {
    return value && value->tv_nsec >= 0 && value->tv_nsec < 1000000000L;
}
/* Valid absolute deadlines may be negative (already expired). Saturate only
 * conversion overflow, never reinterpret a timestamp based on its magnitude. */
static inline struct timespec klj_clock_remaining(int clock, const struct timespec *deadline) {
    struct timespec now, result = {0, 0};
    if (klj_clock_read(clock, &now)) return result;
    if (deadline->tv_sec < now.tv_sec ||
        (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec)) return result;
    __int128 seconds = (__int128)deadline->tv_sec - now.tv_sec;
    long nanos = deadline->tv_nsec - now.tv_nsec;
    if (nanos < 0) { --seconds; nanos += 1000000000L; }
    if (seconds > INT64_MAX) return (struct timespec){INT64_MAX, 999999999};
    return (struct timespec){(time_t)seconds, nanos};
}
static inline struct timespec klj_realtime_after(struct timespec delta) {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    __int128 seconds = (__int128)now.tv_sec + delta.tv_sec;
    long nanos = now.tv_nsec + delta.tv_nsec;
    if (nanos >= 1000000000L) { ++seconds; nanos -= 1000000000L; }
    if (seconds > INT64_MAX) return (struct timespec){INT64_MAX, 999999999};
    return (struct timespec){(time_t)seconds, nanos};
}
#endif
