#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KLDisplayClock KLDisplayClock;
typedef struct KLDisplayTick {
    uint64_t sequence;
    uint64_t callbackTimeNanos;
    uint64_t predictedOutputTimeNanos;
    uint64_t intervalNanos;
    uint64_t coalescedTicks;
    uint32_t displayId;
} KLDisplayTick;
/* Window binding, polling and retirement run on the AppKit main/UI owner.
 * set_demand is serialized and may run on the native display dispatcher;
 * source startup must not depend on a UI callback waiting for a native frame.
 * cocoaWindow is borrowed only for this call; never retained by the clock.
 * Creation does not start a display link. No fallback clock is used. */
KLDisplayClock* kl_display_clock_create(void* cocoaWindow,char* error,size_t errorCapacity);
int kl_display_clock_set_demand(KLDisplayClock*,int wanted,char* error,size_t errorCapacity);
/* Optional precreated native wake. Install only while stopped. The callback
 * must do no JNI/allocating/locking/waiting; the host supplies a coalesced
 * nonblocking Mach semaphore signal and retains its owner until clock stop. */
int kl_display_clock_set_wake(KLDisplayClock*,void (*signal)(void*),void* owner,char* error,size_t errorCapacity);
/* Called for actual SDL_EVENT_WINDOW_DISPLAY_CHANGED, before pumping another tick. */
int kl_display_clock_update_display(KLDisplayClock*,void* cocoaWindow,char* error,size_t errorCapacity);
/* 1: latest unread actual callback; 0: none. Output is untouched on 0. */
int kl_display_clock_poll(KLDisplayClock*,KLDisplayTick*);
/* Independent native dispatcher cursor. Does not consume the UI mailbox or
 * call AppKit/JNI. The caller must retire before the clock is destroyed. */
int kl_display_clock_sample(KLDisplayClock*,uint64_t* cursor,KLDisplayTick*);
/* Stop+release synchronizes CoreVideo callback retirement before freeing userdata.
 * On a genuine Stop failure the object remains owned and must be retried; it is
 * not freed underneath a still-running callback. Returns 1 on destruction. */
int kl_display_clock_destroy(KLDisplayClock*,char* error,size_t errorCapacity);
uint64_t kl_display_clock_monotonic_now(void);
#ifdef __cplusplus
}
#endif
