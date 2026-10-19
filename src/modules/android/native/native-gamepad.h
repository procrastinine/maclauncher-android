#ifndef KLJ_NATIVE_GAMEPAD_H
#define KLJ_NATIVE_GAMEPAD_H
#include <jni.h>
#include <SDL3/SDL.h>
typedef struct KLJGamepads KLJGamepads;
/* All operations belong to the window's SDL event thread. */
KLJGamepads *klj_gamepads_create(JNIEnv *env,SDL_Window *window);
/* 1: controller event consumed; 0: unrelated/window focus event; -1: JNI failure. */
int klj_gamepads_event(JNIEnv *env,KLJGamepads *owner,const SDL_Event *event);
void klj_gamepads_close(JNIEnv *env,KLJGamepads *owner);
#endif
