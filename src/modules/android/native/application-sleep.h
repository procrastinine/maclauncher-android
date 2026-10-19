#pragma once
#include <SDL3/SDL.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KLApplicationSleep KLApplicationSleep;
/* The existing window owner borrows workspace notifications for its lifetime.
 * Events carry only the SDL window ID, never a pointer into a retired owner. */
KLApplicationSleep *kl_application_sleep_observe(SDL_WindowID window);
void kl_application_sleep_retire(KLApplicationSleep *observer);
int kl_application_sleep_event(const SDL_Event *event,SDL_WindowID window,int *sleeping);
#ifdef __cplusplus
}
#endif
