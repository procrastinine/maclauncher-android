#pragma once
#include <jni.h>
#include <SDL3/SDL.h>
#include "session-core.h"
#include "scheduler.h"
#ifdef __cplusplus
extern "C" {
#endif
/* UI owner only. The session retains window/layer; a branch borrows them once. */
int klj_session_claim(JNIEnv*,KLSessionBranch,int,int,SDL_Window**,SDL_MetalView*);
/* Borrowed NSView of the current window; null before a session is ready. */
void* klj_session_native_content(JNIEnv*);
/* One native menu action, dispatched by the existing owner pump. */
int klj_session_bind_activity(JNIEnv*,jobject);
/* Borrowed callback table; its global classes live until this session closes. */
KLHostCallbacks* klj_session_callbacks(JNIEnv*);
int klj_session_stop_notifications(JNIEnv*);
int klj_session_begin_close(JNIEnv*);
/* Cleanup only: bounded <=1s owner pump; failure retains the claimed window. */
int klj_session_quiesce(JNIEnv*,KLHostTaskPump,void* context);
/* Producer retirement precedes callback shutdown; both share the owner pump. */
int klj_session_quiesce_producer(JNIEnv*,KLHostTaskPump,void* context,int (*retire)(void*),void* producer);
int klj_session_release(JNIEnv*,KLSessionBranch,int producerDrained);
int klj_session_committed(JNIEnv*,KLSessionBranch,int,int);
int klj_session_committed_output(JNIEnv*,KLSessionBranch,int sourceWidth,int sourceHeight,int outputWidth,int outputHeight);
int klj_session_source_size(JNIEnv*,int outputWidth,int outputHeight,int *sourceWidth,int *sourceHeight);
int klj_session_output_size(JNIEnv*,int*,int*);
int klj_session_input(JNIEnv*,float*,float*);
int klj_session_display_changed(JNIEnv*);
/* All frame delivery comes from CVDisplayLink. These only dispatch/acknowledge
 * the bounded mailbox and retain the renderer's existing idle deadline. */
int klj_session_pump(JNIEnv*);
int klj_session_event(JNIEnv*,const SDL_Event*);
int klj_session_wait(JNIEnv*,int existingWaitMillis);
/* Native text enters the original active Android input connection. */
int klj_session_text_event(JNIEnv*, const SDL_Event*);
#ifdef __cplusplus
}
#endif

