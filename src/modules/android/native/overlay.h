#pragma once
#include <jni.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private copied host integration. Attach on the window owner before starting
 * the producer thread. All other UI calls use that same owner and the host's
 * validated opaque Java lease; a Java long is never cast to this C pointer.
 * Native and Canvas updates share the same presenter. Canvas changes can
 * redraw over a completed GPU snapshot without waiting for a game frame. */
/* Every int-returning API below uses 0=OK, 3=BUSY, 4=FAILED; never boolean1.
 * Attach and pure size validation return only0 or4. */
int klj_metal_overlay_attach(JNIEnv *,void *presenter,int width,int height);
int klj_metal_overlay_validate_size(JNIEnv *,void *presenter,int width,int height);
int klj_metal_overlay_submit(JNIEnv *,void *presenter,jintArray,int offset,int stride,int width,int height,uint64_t uiFrameId);
int klj_metal_overlay_clear(JNIEnv *,void *presenter,uint64_t uiFrameId);

/* Private implementation seam, also used by the deterministic native fixture.
 * Status: 0 accepted, 3 transient busy, 4 failure with a Java exception when env
 * is provided. BUSY/failed upload leaves its ID reusable. No JNI array retained.
 * set_surface/encode run on the native producer; submit/clear on the UI owner.
 * Destroy only after UI submission and producer calls have stopped. GPU work
 * may remain in flight: completion/abandonment leases own exact resources. */
void *klj_overlay_create(JNIEnv *,int width,int height);
int klj_overlay_validate_size(JNIEnv *,void *,int width,int height);
int klj_overlay_set_surface(JNIEnv *,void *,void *metalDevice,int width,int height);
int klj_overlay_submit(JNIEnv *,void *,jintArray,int offset,int stride,int width,int height,uint64_t uiFrameId);
int klj_overlay_clear(JNIEnv *,void *,uint64_t uiFrameId);
typedef struct {void *texture;uint64_t uiFrameId;int uiVisible;} KLJOverlayFrame;
/* Encode after the existing produced-event wait, before shared effects.
 * Empty UI returns source unchanged and encodes no pass or copy.
 * Nonempty UI returns a distinct retained composite. On BUSY, keep the prior
 * displayed frame, commit only the existing consumed-event signal, and skip
 * submission to the presenter. Never display an uncomposited replacement. */
/* Canvas pixels use top-left origin. Match the producer's actual origin before
 * the common presenter applies its single display transform. */
int klj_overlay_encode(JNIEnv *,void *,void *retainedUnsubmittedCommandBuffer,void *sourceTexture,int sourceTopLeft,KLJOverlayFrame *);
/* Redraw the current UI over the latest completed immutable native image.
 * BUSY retains the pending Canvas update until a base is available. An OK
 * frame with no texture means an empty UI is already absent from presentation.
 * Acknowledge only after the common presenter accepts the encoded frame. */
int klj_overlay_encode_cached(JNIEnv *,void *,void *retainedUnsubmittedCommandBuffer,KLJOverlayFrame *);
void klj_overlay_frame_submitted(void *,const KLJOverlayFrame *);
/* Retain the latest Canvas pixels through this command buffer's completion.
 * No source readback or additional copy is needed for a visible UI image. */
int klj_overlay_encode_canvas(JNIEnv *,void *,void *retainedUnsubmittedCommandBuffer,void **sourceTexture);
void klj_overlay_destroy(void *);
typedef struct {uint64_t bytes,peakBytes,uploaded,busy,composed,bypassed,cleared,lastUiId;unsigned uiSlots,compositeSlots,inFlight,baseSlots;} KLJOverlayStats;
int klj_overlay_stats(void *,KLJOverlayStats *);
#ifdef __cplusplus
}
#endif
