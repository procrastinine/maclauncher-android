#pragma once
#include <errno.h>
#include <stdint.h>
#include <sys/time.h>

/* Bionic arm64 timeval has two signed 64-bit fields. Darwin's microseconds
 * field is 32-bit followed by padding; forwarding the pointer corrupts reads
 * and silently truncates invalid input. Timers remain owned by the kernel. */
struct klj_timer_timeval { int64_t seconds, microseconds; };
struct klj_timer_value { struct klj_timer_timeval interval, value; };
static int klj_timer_selector(int which) {
    switch(which) { case 0:return ITIMER_REAL; case 1:return ITIMER_VIRTUAL; case 2:return ITIMER_PROF; }
    errno=EINVAL;return -1;
}
static int klj_timer_import(const struct klj_timer_timeval *guest,struct timeval *host) {
    if(guest->seconds<0||guest->microseconds<0||guest->microseconds>=1000000){errno=EINVAL;return -1;}
    host->tv_sec=guest->seconds;host->tv_usec=(suseconds_t)guest->microseconds;return 0;
}
static void klj_timer_export(const struct itimerval *host,struct klj_timer_value *guest) {
    guest->interval=(struct klj_timer_timeval){host->it_interval.tv_sec,host->it_interval.tv_usec};
    guest->value=(struct klj_timer_timeval){host->it_value.tv_sec,host->it_value.tv_usec};
}
static int klj_getitimer(int which,struct klj_timer_value *value) {
    int native=klj_timer_selector(which);if(native<0)return -1;
    if(!value){errno=EFAULT;return -1;}
    struct itimerval actual;if(getitimer(native,&actual))return -1;
    klj_timer_export(&actual,value);return 0;
}
static int klj_setitimer(int which,const struct klj_timer_value *value,struct klj_timer_value *old) {
    int native=klj_timer_selector(which);if(native<0)return -1;
    struct itimerval requested={0},previous;
    /* Linux accepts a null new value as disarming the timer. */
    if(value&&(klj_timer_import(&value->interval,&requested.it_interval)||klj_timer_import(&value->value,&requested.it_value)))return -1;
    if(setitimer(native,&requested,old?&previous:NULL))return -1;
    if(old)klj_timer_export(&previous,old);return 0;
}
