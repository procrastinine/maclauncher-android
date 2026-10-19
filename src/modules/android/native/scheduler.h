#pragma once
#include <jni.h>
#include <SDL3/SDL.h>
typedef struct KLHostScheduler KLHostScheduler;
typedef struct KLHostCallbacks KLHostCallbacks;
typedef void (*KLHostTaskPump)(JNIEnv*,void* context);
#ifdef __cplusplus
extern "C" {
#endif
/* One active scheduler and one game/SDL event-subsystem lifetime per JVM.
 * SDL_Quit is final process-host teardown, never between scheduler instances. */
KLHostScheduler* kl_host_scheduler_create(JNIEnv*,KLHostCallbacks*);
int kl_host_scheduler_window(JNIEnv*,KLHostScheduler*,void* cocoaWindow);
int kl_host_scheduler_changed_display(JNIEnv*,KLHostScheduler*,void* cocoaWindow);
int kl_host_scheduler_pump(JNIEnv*,KLHostScheduler*);
/* Acknowledges this host's coalesced SDL wake; event contains no owner pointer. */
int kl_host_scheduler_event(KLHostScheduler*,const SDL_Event*);
/* <= existingWait, limited only by an actual delayed registration deadline. */
int kl_host_scheduler_wait(JNIEnv*,KLHostScheduler*,int existingWait);
/* Keep actual display callbacks available while synchronous Activity teardown
 * waits for its render thread. Call quiesce only after that handoff finishes. */
int kl_host_scheduler_begin_close(JNIEnv*,KLHostScheduler*);
/* Stop source, clear Java work, wait <= timeoutMillis for already-active work.
 * A failure prohibits guest/native resource destruction and another host run.
 * A preexisting JNI exception is preserved while cleanup still runs. Return
 * value reports retirement; a preserved exception may remain pending on 1. */
int kl_host_scheduler_quiesce(JNIEnv*,KLHostScheduler*,unsigned timeoutMillis);
/* Optional existing UI task pump permits an in-flight worker callback to finish
 * a previously queued owner task after new frame delivery has stopped. */
int kl_host_scheduler_quiesce_pump(JNIEnv*,KLHostScheduler*,unsigned timeoutMillis,KLHostTaskPump,void* context);
int kl_host_scheduler_destroy(JNIEnv*,KLHostScheduler*);
#ifdef __cplusplus
}
#endif
