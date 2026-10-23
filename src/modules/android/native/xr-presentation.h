#pragma once
#include <jni.h>
#ifdef __cplusplus
extern "C" {
#endif
void klj_xr_provider_install(void);
void *klj_xr_attach(JNIEnv*,void *presenter,void *layer);
int klj_xr_pump(JNIEnv*,void*,int width,int height);
int klj_xr_retire(void*);
void klj_xr_detach(void*);
/* Arguments are the existing SDL_Window and its already-polled SDL_Event. */
int klj_xr_input_active(void*);
int klj_xr_event(void*,void *window,const void *event,int enabled);
void klj_xr_controls(void*,void *window,int enabled);
#ifdef __cplusplus
}
#endif
