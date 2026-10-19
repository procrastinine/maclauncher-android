#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Borrow actual ANGLE state. These functions never create or switch a context. */
int klj_glfb_configure(void *(*resolver)(const char *));
/* Explicit GLSurfaceView request to initialize the existing owned GLFB context.
 * Refuses to replace an already-borrowed guest context. */
int klj_glfb_init_owned(void);
void klj_glfb_context_created(void *display,void *context,void *shared);
void klj_glfb_context_destroyed(void *context);
void klj_glfb_current(void *display,void *context);
void klj_glfb_consumer(void *event);
uint64_t klj_glfb_retire_consumer(void);
int klj_glfb_consumer_failed(void);
void *klj_glfb_display(void);
#ifdef __cplusplus
}
#endif
