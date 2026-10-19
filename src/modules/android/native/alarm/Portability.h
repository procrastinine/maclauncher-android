#pragma once
#define EPOLL_SHIM_DISABLE_WRAPPER_MACROS 1
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include "compat_itimerspec.h"
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include "../pollable-fds/bridge.h"

// AlarmManager's original JNI module uses the descriptor implementation already
// owned by the runtime. Only Darwin clock/flag representations differ here.
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM 9
#endif
extern "C" int kl_errno_to_linux(int);

template<class Function> inline Function alarmDescriptor(const char* name) {
    return reinterpret_cast<Function>(klj_pollable_lookup(name));
}
inline int alarmEpollCreate(int size) {
    return alarmDescriptor<int(*)(int)>("epoll_create")(size);
}
inline int alarmEpollCtl(int owner,int operation,int fd,epoll_event* event) {
    return alarmDescriptor<int(*)(int,int,int,epoll_event*)>("epoll_ctl")(owner,operation,fd,event);
}
inline int alarmEpollWait(int owner,epoll_event* events,int count,int timeout) {
    return alarmDescriptor<int(*)(int,epoll_event*,int,int)>("epoll_wait")(owner,events,count,timeout);
}
inline int alarmTimerCreate(int clock,int flags) {
    int androidClock;
    switch(static_cast<int>(clock)) {
        case CLOCK_REALTIME: androidClock=0;break;
        case CLOCK_MONOTONIC: androidClock=1;break;
        case CLOCK_BOOTTIME: androidClock=7;break;
        case CLOCK_REALTIME_ALARM: androidClock=8;break;
        case CLOCK_BOOTTIME_ALARM: androidClock=9;break;
        default: errno=EINVAL;return -1;
    }
    if(flags&~(O_CLOEXEC|O_NONBLOCK)){errno=EINVAL;return -1;}
    int androidFlags=((flags&O_CLOEXEC)?0x80000:0)|((flags&O_NONBLOCK)?0x800:0);
    return alarmDescriptor<int(*)(int,int)>("timerfd_create")(androidClock,androidFlags);
}
inline int alarmTimerSet(int fd,int flags,const itimerspec* next,itimerspec* previous) {
    return alarmDescriptor<int(*)(int,int,const itimerspec*,itimerspec*)>("timerfd_settime")(fd,flags,next,previous);
}
inline int alarmTimerGet(int fd,itimerspec* next) {
    return alarmDescriptor<int(*)(int,itimerspec*)>("timerfd_gettime")(fd,next);
}
#define epoll_create alarmEpollCreate
#define epoll_ctl alarmEpollCtl
#define epoll_wait alarmEpollWait
#define timerfd_create alarmTimerCreate
#define timerfd_settime alarmTimerSet
#define timerfd_gettime alarmTimerGet
#define read klj_pollable_read
#define close klj_pollable_close
