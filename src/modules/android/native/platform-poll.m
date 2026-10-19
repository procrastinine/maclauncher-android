#import <AppKit/AppKit.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

/* ALooper remains the descriptor and callback owner. On the macOS main thread,
 * its blocking wait must also service AppKit, including after the last Android
 * window closes while a service continues running. Scoped CF sources only wake
 * that wait: poll still supplies the original readiness/error results. */
static double monotonic_seconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void descriptor_ready(CFFileDescriptorRef descriptor,
                             CFOptionFlags events, void *context) {
    (void)descriptor; (void)events; (void)context;
}

typedef struct {
    CFFileDescriptorRef descriptor;
    CFRunLoopSourceRef source;
    CFOptionFlags events;
} descriptor_watch;

static void dispatch_appkit(void) {
    NSEvent *event;
    while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode dequeue:YES])) {
        [NSApp sendEvent:event];
    }
    [NSApp updateWindows];
}

int klj_platform_poll(struct pollfd *fds, nfds_t count, int timeout) {
    if (!pthread_main_np() || !NSApp) return poll(fds, count, timeout);
    @autoreleasepool {
        // Already-ready Android descriptors must not starve Cocoa either.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        dispatch_appkit();
    }
    int result = poll(fds, count, 0);
    if (result || !timeout) return result;
    double deadline = timeout < 0 ? 0 : monotonic_seconds() + timeout / 1000.0;
    descriptor_watch *watches = calloc(count ? count : 1, sizeof(*watches));
    if (!watches) { errno = ENOMEM; return -1; }
    CFRunLoopRef loop = CFRunLoopGetCurrent();
    int saved_error = 0;
    for (nfds_t i = 0; i < count; i++) {
        if (fds[i].fd < 0) continue;
        // Read monitoring also observes hangup/error on descriptors which only
        // requested those implicit poll events. No descriptor is closed here.
        watches[i].events = (fds[i].events & POLLOUT ? kCFFileDescriptorWriteCallBack : 0)
                         | (fds[i].events & (POLLIN | POLLPRI) || !fds[i].events
                            ? kCFFileDescriptorReadCallBack : 0);
        if (!watches[i].events) watches[i].events = kCFFileDescriptorReadCallBack;
        watches[i].descriptor = CFFileDescriptorCreate(NULL, fds[i].fd, false,
                                                       descriptor_ready, NULL);
        if (!watches[i].descriptor) { saved_error = ENOMEM; result = -1; break; }
        watches[i].source = CFFileDescriptorCreateRunLoopSource(NULL, watches[i].descriptor, 0);
        if (!watches[i].source) { saved_error = ENOMEM; result = -1; break; }
        CFRunLoopAddSource(loop, watches[i].source, kCFRunLoopDefaultMode);
    }
    while (!result) {
        @autoreleasepool {
            // AppKit's run-loop sources receive events; sendEvent performs their
            // normal dispatch. A service with no windows still needs this pump.
            dispatch_appkit();
            result = poll(fds, count, 0);
            if (result) { saved_error = errno; break; }
            double remaining = timeout < 0 ? 1e10 : deadline - monotonic_seconds();
            if (remaining <= 0) break;
            for (nfds_t i = 0; i < count; i++)
                if (watches[i].descriptor)
                    CFFileDescriptorEnableCallBacks(watches[i].descriptor, watches[i].events);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, remaining, true);
        }
    }
    for (nfds_t i = 0; i < count; i++) {
        if (watches[i].source) {
            CFRunLoopRemoveSource(loop, watches[i].source, kCFRunLoopDefaultMode);
            CFRelease(watches[i].source);
        }
        if (watches[i].descriptor) {
            CFFileDescriptorInvalidate(watches[i].descriptor);
            CFRelease(watches[i].descriptor);
        }
    }
    free(watches);
    if (result < 0) errno = saved_error;
    return result;
}
