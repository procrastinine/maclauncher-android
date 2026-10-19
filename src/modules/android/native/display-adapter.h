#pragma once
#include <jni.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct KLDisplayAdapter KLDisplayAdapter;
KLDisplayAdapter* kl_display_adapter_create(JNIEnv*);
/* Before any original Display mode query. Reinvoke only for an actual screen,
 * current-mode, or connection event; unchanged snapshots enqueue nothing. */
/* surfaceWidth/Height MUST be the committed EGL/Canvas/IOSurface pixel extent,
 * not NSWindow points, monitor pixels, or an uncommitted resize event. */
int kl_display_adapter_update(JNIEnv*,KLDisplayAdapter*,void* cocoaWindow,int surfaceWidth,int surfaceHeight);
/* For resize, invoke update again only after native storage commits. Legacy
 * Display.hostViewport is bootstrap-only and cannot mutate a bound mode. */
/* Stop host display-event production and retire Java listeners. The actual
 * still-owned window and its committed Display snapshot remain valid through
 * guest producer shutdown.
 * Returns 0 while callbacks are in flight; caller retains guest resources and
 * may recheck after owner-thread callbacks finish. Preserves pending exception. */
int kl_display_adapter_stop(JNIEnv*,KLDisplayAdapter*);
/* Legal only after stop succeeds and the guest producer retires. Invalidates
 * the logical Display immediately before window teardown, then releases exactly
 * the two class global refs. Preserves an existing exception while cleaning up. */
int kl_display_adapter_destroy(JNIEnv*,KLDisplayAdapter*);
#ifdef __cplusplus
}
#endif
