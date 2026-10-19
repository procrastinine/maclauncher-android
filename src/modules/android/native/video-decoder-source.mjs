function replace(source,before,after){
  if(source.split(before).length!==2)throw Error('Pinned VideoToolbox source changed: '+before.slice(0,100));
  return source.replace(before,after);
}
function section(source,begin,end,body){
  const start=source.indexOf(begin),stop=source.indexOf(end,start+begin.length);
  if(start<0||stop<0)throw Error('Pinned VideoToolbox function changed: '+begin);
  return source.slice(0,start)+body+'\n\n'+source.slice(stop);
}

export function videoDecoderHeader(source){
  return replace(source,'\n#endif',`
// The codec owner retains the decoder during each call and serializes session
// operations. Notifications only wake/post its worker; they must never invoke
// decoder lifecycle operations synchronously from VideoToolbox's callback.
// Replacing/detaching a notification waits until the old callback is finished.
void klj_vtdec_set_callback(kl_vtdec *, void (*notify)(void *), void *context);
// Wait: 1 frame, 0 timeout, actual OSStatus/-errno on error. Negative timeout
// waits indefinitely. Flush interrupts pending waits with -EINTR; close uses
// -ECANCELED. No polling and no frame reference is acquired by waiting.
int klj_vtdec_wait(kl_vtdec *, int64_t timeout_us);
int klj_vtdec_status(kl_vtdec *);
void klj_vtdec_close(kl_vtdec *);
int klj_vtdec_drain(kl_vtdec *);
// Complete submitted device work without draining reorder/reference state.
int klj_vtdec_complete(kl_vtdec *);

#endif`);
}

const ownership=String.raw`
static pthread_mutex_t g_last_lock = PTHREAD_MUTEX_INITIALIZER;

static void klvt_notify(kl_vtdec *d) {
    void (*notify)(void *) = NULL;
    void *context = NULL;
    pthread_mutex_lock(&d->lock);
    if (!d->closing && !d->flushing && d->notify) {
        notify = d->notify; context = d->notify_context; d->notifying++;
    }
    pthread_mutex_unlock(&d->lock);
    if (!notify) return;
    notify(context);
    pthread_mutex_lock(&d->lock);
    d->notifying--;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
}

static int klvt_error(kl_vtdec *d, int error) {
    if (!error) return 0;
    pthread_mutex_lock(&d->lock);
    if (!d->closing && !d->flushing) {
        if (!d->error) d->error = error;
        d->n_failed++;
        pthread_cond_broadcast(&d->changed);
    }
    pthread_mutex_unlock(&d->lock);
    klvt_notify(d);
    return error;
}

int klj_vtdec_status(kl_vtdec *d) {
    if (!d) return -EINVAL;
    pthread_mutex_lock(&d->lock);
    int status = d->closing ? -ECANCELED : d->error;
    pthread_mutex_unlock(&d->lock);
    return status;
}

void klj_vtdec_set_callback(kl_vtdec *d, void (*notify)(void *), void *context) {
    if (!d) return;
    pthread_mutex_lock(&d->callback_lock);
    pthread_mutex_lock(&d->lock);
    d->notify = NULL; d->notify_context = NULL;
    while (d->notifying) pthread_cond_wait(&d->changed, &d->lock);
    if (!d->closing) { d->notify = notify; d->notify_context = context; }
    int ready = !d->closing && (d->head != NULL || d->error);
    pthread_mutex_unlock(&d->lock);
    pthread_mutex_unlock(&d->callback_lock);
    if (ready) klvt_notify(d);
}

void klj_vtdec_close(kl_vtdec *d) {
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    d->closing = 1;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
    klj_vtdec_set_callback(d, NULL, NULL);
}

static uint64_t klvt_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}
static void klvt_wait_done(void *context) {
    kl_vtdec *d = context;
    d->waiters--;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
}
int klj_vtdec_wait(kl_vtdec *d, int64_t timeout_us) {
    if (!d) return -EINVAL;
    uint64_t now = klvt_now_us(), deadline = UINT64_MAX;
    if (timeout_us >= 0 && (uint64_t)timeout_us <= UINT64_MAX - now)
        deadline = now + (uint64_t)timeout_us;
    int result = 0;
    pthread_mutex_lock(&d->lock);
    uint64_t epoch = d->epoch;
    d->waiters++;
    pthread_cleanup_push(klvt_wait_done, d);
    for (;;) {
        if (d->closing) { result = -ECANCELED; break; }
        if (d->flushing || epoch != d->epoch) { result = -EINTR; break; }
        if (d->head) { result = 1; break; }
        if (d->error) { result = d->error; break; }
        int error;
        if (timeout_us < 0) error = pthread_cond_wait(&d->changed, &d->lock);
        else {
            now = klvt_now_us();
            if (now >= deadline) break;
            uint64_t remaining = deadline - now;
            struct timespec relative = { (time_t)(remaining / 1000000),
                                        (long)(remaining % 1000000) * 1000 };
            error = pthread_cond_timedwait_relative_np(&d->changed, &d->lock, &relative);
        }
        if (error == ETIMEDOUT) continue;
        if (error) { result = -error; break; }
    }
    pthread_cleanup_pop(1);
    return result;
}

static void klvt_free_frames(klvt_frame *frame) {
    while (frame) {
        klvt_frame *next = frame->next;
        CVPixelBufferRelease(frame->pb);
        free(frame); frame = next;
    }
}
`;

export function videoDecoderSource(source){
  source=replace(source,'a small ring the guest pulls from','an owned FIFO the codec pulls from');
  source=replace(source,'#include <pthread.h>','#include <pthread.h>\n#include <errno.h>\n#include <time.h>');
  source=replace(source,'#define KLVT_RING 16            // decoded frames held before the oldest is dropped\n','');
  source=replace(source,'typedef struct { CVPixelBufferRef pb; int64_t pts_us; } klvt_frame;',
    'typedef struct klvt_frame { CVPixelBufferRef pb; int64_t pts_us; struct klvt_frame *next; } klvt_frame;');
  source=replace(source,`    pthread_mutex_t lock;       // guards the ring and the counters only; the
                                // session is touched from the submitting thread
    klvt_frame ring[KLVT_RING];
    int ring_head, ring_count;`, `    pthread_mutex_t lock;       // FIFO, callback ownership, waiters and status
    pthread_mutex_t callback_lock;
    pthread_cond_t changed;
    klvt_frame *head, *tail;
    size_t queued;
    uint64_t epoch;
    unsigned waiters, notifying;
    int closing, flushing, error;
    void (*notify)(void *);
    void *notify_context;`);
  source=replace(source,'static kl_vtdec *g_last;        // for kl_vtdec_report; the guest makes one',
    'static kl_vtdec *g_last;        // diagnostic pointer guarded by g_last_lock\n'+ownership);
  source=section(source,'static void klvt_output(', '// Build the format description',String.raw`static void klvt_output(void *decompressionOutputRefCon, void *sourceFrameRefCon,
                        OSStatus status, VTDecodeInfoFlags flags,
                        CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)sourceFrameRefCon; (void)duration;
    kl_vtdec *d = decompressionOutputRefCon;
    if (status != noErr) { klvt_error(d, status); return; }
    if (!image) {
        if (flags & kVTDecodeInfo_FrameDropped) {
            pthread_mutex_lock(&d->lock); d->n_dropped++; pthread_mutex_unlock(&d->lock);
        }
        return;
    }
    if (!CMTIME_IS_NUMERIC(pts)) { klvt_error(d, -EINVAL); return; }
    klvt_frame *frame = malloc(sizeof *frame);
    if (!frame) { klvt_error(d, -ENOMEM); return; }
    frame->pb = (CVPixelBufferRef)CFRetain(image);
    frame->pts_us = CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
    frame->next = NULL;
    pthread_mutex_lock(&d->lock);
    if (d->closing || d->flushing) {
        pthread_mutex_unlock(&d->lock); klvt_free_frames(frame); return;
    }
    d->n_decoded++;
    d->width = (int)CVPixelBufferGetWidth(image);
    d->height = (int)CVPixelBufferGetHeight(image);
    if (d->tail) d->tail->next = frame; else d->head = frame;
    d->tail = frame; d->queued++;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
    klvt_notify(d);
}`);
  source=replace(source,'our ring; the count is the reference client\'s.','the output queue; the count is the reference client\'s.');
  source=replace(source,'    CFDictionarySetValue(attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);',
    '    if (!attrs) return klvt_error(d, -ENOMEM);\n    CFDictionarySetValue(attrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);');
  source=replace(source,'        CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, pfn);',
    '        if (!pfn) { CFRelease(attrs); return klvt_error(d, -ENOMEM); }\n        CFDictionarySetValue(attrs, kCVPixelBufferPixelFormatTypeKey, pfn);');
  source=replace(source,'        CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, empty);',
    '        if (!empty) { CFRelease(attrs); return klvt_error(d, -ENOMEM); }\n        CFDictionarySetValue(attrs, kCVPixelBufferIOSurfacePropertiesKey, empty);');
  source=replace(source,'        CFDictionarySetValue(attrs, kCVPixelBufferPoolMinimumBufferCountKey, mcn);',
    '        if (!mcn) { CFRelease(attrs); return klvt_error(d, -ENOMEM); }\n        CFDictionarySetValue(attrs, kCVPixelBufferPoolMinimumBufferCountKey, mcn);');
  source=replace(source,'    pthread_mutex_init(&d->lock, NULL);',`    int error = pthread_mutex_init(&d->lock, NULL);
    if (error) { free(d); errno = error; return NULL; }
    error = pthread_mutex_init(&d->callback_lock, NULL);
    if (error) { pthread_mutex_destroy(&d->lock); free(d); errno = error; return NULL; }
    error = pthread_cond_init(&d->changed, NULL);
    if (error) { pthread_mutex_destroy(&d->callback_lock); pthread_mutex_destroy(&d->lock); free(d); errno = error; return NULL; }`);
  source=replace(source,'    g_last = d;', '    pthread_mutex_lock(&g_last_lock); g_last = d; pthread_mutex_unlock(&g_last_lock);');
  source=section(source,'void kl_vtdec_destroy(', '// The leading ue(v)',String.raw`void kl_vtdec_destroy(kl_vtdec *d) {
    if (!d) return;
    pthread_mutex_lock(&g_last_lock);
    if (g_last == d) g_last = NULL;
    pthread_mutex_unlock(&g_last_lock);
    klj_vtdec_close(d);
    klvt_teardown_session(d);
    pthread_mutex_lock(&d->lock);
    while (d->waiters) pthread_cond_wait(&d->changed, &d->lock);
    klvt_frame *frames = d->head;
    d->head = d->tail = NULL; d->queued = 0;
    pthread_mutex_unlock(&d->lock);
    klvt_free_frames(frames);
    if (d->dump) fclose(d->dump);
    if (d->dump_idx) fclose(d->dump_idx);
    for (int i = 0; i < KLVT_NSETS; i++) free(d->set[i]);
    pthread_cond_destroy(&d->changed);
    pthread_mutex_destroy(&d->callback_lock);
    pthread_mutex_destroy(&d->lock);
    free(d);
}`);
  source=replace(source,'    if (!copy) return 0;','    if (!copy) return klvt_error(d, -ENOMEM);');
  source=replace(source,'    if (!d || !annexb || !len) return -1;',`    if (!d) return -EINVAL;
    if (!annexb || !len) return klvt_error(d, -EINVAL);
    int previous = klj_vtdec_status(d);
    if (previous) return previous;`);
  source=replace(source,'            d->sets_changed |= klvt_hold(d, slot, nal, nal_len);',`            int changed = klvt_hold(d, slot, nal, nal_len);
            if (changed < 0) return changed;
            d->sets_changed |= changed;`);
  source=replace(source,'        payload += KLVT_NAL_LEN_BYTES + nal_len;',`        if (nal_len > UINT32_MAX || nal_len > SIZE_MAX - KLVT_NAL_LEN_BYTES ||
            payload > SIZE_MAX - KLVT_NAL_LEN_BYTES - nal_len) return klvt_error(d, -EOVERFLOW);
        payload += KLVT_NAL_LEN_BYTES + nal_len;`);
  source=replace(source,'        if (klvt_rebuild(d) < 0) return -1;','        int status = klvt_rebuild(d);\n        if (status) return status;');
  source=replace(source,'    if (!buf) return -1;','    if (!buf) return klvt_error(d, -ENOMEM);');
  source=replace(source,'    if (st != noErr) { free(buf); return -1; }','    if (st != noErr) { free(buf); return klvt_error(d, st); }');
  source=replace(source,'    if (st != noErr) return -1;','    if (st != noErr) return klvt_error(d, st);');
  // Each remaining generic failure has the original OSStatus in scope.
  if((source.match(/        return -1;/g)||[]).length!==3)throw Error('Pinned VideoToolbox status exits changed');
  source=source.replaceAll('        return -1;','        return klvt_error(d, st);');
  source=section(source,'CVPixelBufferRef kl_vtdec_pull(', 'void kl_vtdec_stats(',String.raw`CVPixelBufferRef kl_vtdec_pull(kl_vtdec *d, int64_t *pts_us_out) {
    if (!d) return NULL;
    pthread_mutex_lock(&d->lock);
    klvt_frame *frame = d->closing || d->flushing ? NULL : d->head;
    if (frame) {
        d->head = frame->next; if (!d->head) d->tail = NULL; d->queued--;
    }
    pthread_mutex_unlock(&d->lock);
    if (!frame) return NULL;
    CVPixelBufferRef pb = frame->pb;
    if (pts_us_out) *pts_us_out = frame->pts_us;
    free(frame);
    return pb; // Transfers the callback's sole queue retain to the caller.
}

int klj_vtdec_complete(kl_vtdec *d) {
    if (!d) return -EINVAL;
    pthread_mutex_lock(&d->lock); int closed = d->closing; pthread_mutex_unlock(&d->lock);
    if (closed) return -ECANCELED;
    if (!d->sess) return klj_vtdec_status(d);
    OSStatus status = VTDecompressionSessionWaitForAsynchronousFrames(d->sess);
    return status ? klvt_error(d, status) : klj_vtdec_status(d);
}

int klj_vtdec_drain(kl_vtdec *d) {
    if (!d) return -EINVAL;
    pthread_mutex_lock(&d->lock); int closed = d->closing; pthread_mutex_unlock(&d->lock);
    if (closed) return -ECANCELED;
    if (!d->sess) return klj_vtdec_status(d);
    OSStatus delayed = VTDecompressionSessionFinishDelayedFrames(d->sess);
    OSStatus pending = VTDecompressionSessionWaitForAsynchronousFrames(d->sess);
    int status = delayed != noErr ? delayed : pending;
    return status ? klvt_error(d, status) : klj_vtdec_status(d);
}

void kl_vtdec_flush(kl_vtdec *d) {
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    if (d->closing) { pthread_mutex_unlock(&d->lock); return; }
    d->flushing = 1; d->epoch++; d->error = 0;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
    int status = klj_vtdec_drain(d);
    // Flush discards decoder reference state too. Retained parameter sets let
    // the original rebuild path recreate the session on the next access unit.
    klvt_teardown_session(d);
    pthread_mutex_lock(&d->lock);
    klvt_frame *frames = d->head;
    d->head = d->tail = NULL; d->queued = 0;
    d->error = status; d->flushing = 0;
    pthread_cond_broadcast(&d->changed);
    pthread_mutex_unlock(&d->lock);
    klvt_free_frames(frames);
    if (status) klvt_notify(d);
}
`);
  source=replace(source,'    kl_vtdec *d = g_last;\n    if (!d) return;',`    pthread_mutex_lock(&g_last_lock);
    kl_vtdec *d = g_last;
    if (!d) { pthread_mutex_unlock(&g_last_lock); return; }`);
  source=replace(source,'               " %d held\\n",','               " %zu held\\n",');
  source=replace(source,'            d->ring_count);','            d->queued);');
  const report=source.indexOf('void kl_vtdec_report(');
  source=source.slice(0,report)+replace(source.slice(report),'    pthread_mutex_unlock(&d->lock);','    pthread_mutex_unlock(&d->lock);\n    pthread_mutex_unlock(&g_last_lock);');
  if(/KLVT_RING|ring_head|ring_count|d->ring/.test(source))throw Error('Fixed decoder ring remains');
  return source;
}
