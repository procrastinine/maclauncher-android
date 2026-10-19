#pragma once
#include "clock-domain.h"
extern int kl_errno_to_linux(int);
/* POSIX returns the positive guest error code and does not change errno. */
static int klj_domain_clock_nanosleep(int clock, int flags, const struct timespec *request, struct timespec *remaining) {
    int saved = errno, result = 0;
    if (!klj_nsec_valid(request) || request->tv_sec < 0 || (flags != 0 && flags != 1)) { result = EINVAL; goto done; }
    if (clock != 0 && clock != 1 && clock != 11) { result = ENOTSUP; goto done; }
    if (!flags) {
        if (nanosleep(request, remaining)) result = errno;
    } else {
        for (;;) {
            struct timespec delta = klj_clock_remaining(clock, request);
            if (!delta.tv_sec && !delta.tv_nsec) break;
            /* Realtime may move; never change remaining on an absolute wait. */
            if ((clock == 0 || clock == 11) && (delta.tv_sec || delta.tv_nsec > 100000000L)) delta = (struct timespec){0, 100000000};
            if (nanosleep(&delta, NULL)) { result = errno; break; }
        }
    }
done:
    result = kl_errno_to_linux(result); errno = saved; return result;
}
