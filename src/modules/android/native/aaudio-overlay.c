// Owned output pause operation over the pinned upstream stream and sink.
// No guest structure is mirrored and no native stream is replaced.
static _Thread_local void *owned_feeding;
#define kl_aaudio_lookup kl_aaudio_base_lookup
#define kl_aaudio_sym kl_aaudio_base_sym
#include KLEPTON_AAUDIO_SOURCE
#undef kl_aaudio_lookup
#undef kl_aaudio_sym

extern int kl_audio_pause_exclusive(const void *source);

// Android's MMAP policies describe its audio-HAL transport. Klepton already
// supplies callback/read/write streams over the Mac audio sink; those streams
// do not use an Android MMAP endpoint. Keep capability probing truthful so
// Oboe/FMOD can select that existing stream implementation.
#include <stdatomic.h>
static _Atomic int32_t owned_mmap_policy;
static int32_t klaa_platformMMapPolicy(int32_t device,int32_t direction){
    (void)device;(void)direction;return 1; // AAUDIO_POLICY_NEVER
}
static int32_t klaa_getMMapPolicy(void){return atomic_load(&owned_mmap_policy);}
static aaudio_result_t klaa_setMMapPolicy(int32_t policy){
    if(policy<0||policy>3)return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    atomic_store(&owned_mmap_policy,policy);return AAUDIO_OK;
}
static aaudio_result_t klaa_openWithPolicy(klaa_builder *builder,klaa_stream **output){
    if(!builder||!output)return AAUDIO_ERROR_NULL;
    if(atomic_load(&owned_mmap_policy)==3){*output=NULL;return AAUDIO_ERROR_NO_SERVICE;}
    return klaa_openStream(builder,output);
}

static aaudio_result_t klaa_requestPause(klaa_stream *stream) {
    if(!stream)return AAUDIO_ERROR_NULL;
    if(stream->direction!=AAUDIO_DIRECTION_OUTPUT)return AAUDIO_ERROR_UNIMPLEMENTED;
    pthread_mutex_lock(&stream->lock);
    if(owned_feeding==stream||(stream->thread_live&&pthread_equal(stream->thread,pthread_self()))){
        pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_INVALID_STATE;
    }
    if(stream->state==AAUDIO_STREAM_STATE_PAUSED){pthread_mutex_unlock(&stream->lock);return AAUDIO_OK;}
    if(stream->state!=AAUDIO_STREAM_STATE_STARTED&&stream->state!=AAUDIO_STREAM_STATE_STARTING){
        pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_INVALID_STATE;
    }
    // The pinned sink mixes outputs globally, so it cannot pause one of two
    // streams independently. Refuse before mutating either producer or sink.
    if(g_open_streams!=1){pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_UNAVAILABLE;}
    int paused=kl_audio_pause_exclusive(stream);
    if(paused!=1){pthread_mutex_unlock(&stream->lock);return paused<0?AAUDIO_ERROR_INTERNAL:AAUDIO_ERROR_UNAVAILABLE;}
    int live=stream->thread_live;pthread_t thread=stream->thread;
    klj_audio_activity_transition(3,stream->state==AAUDIO_STREAM_STATE_STARTED,0);
    stream->state=AAUDIO_STREAM_STATE_PAUSING;stream->running=0;
    pthread_mutex_unlock(&stream->lock);
    if(live&&pthread_join(thread,NULL)!=0)return AAUDIO_ERROR_INTERNAL;
    pthread_mutex_lock(&stream->lock);
    stream->thread_live=0;
    if(stream->state!=AAUDIO_STREAM_STATE_PAUSING){pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_INVALID_STATE;}
    stream->state=AAUDIO_STREAM_STATE_PAUSED;
    pthread_mutex_unlock(&stream->lock);
    return AAUDIO_OK;
}

void *kl_aaudio_lookup(const char *name){
    if(name&&!strcmp(name,"AAudioStream_requestPause"))return (void*)klaa_requestPause;
    if(name&&(!strcmp(name,"AAudio_getPlatformMMapPolicy")||!strcmp(name,"AAudio_getPlatformMMapExclusivePolicy")))return (void*)klaa_platformMMapPolicy;
    if(name&&!strcmp(name,"AAudio_getMMapPolicy"))return (void*)klaa_getMMapPolicy;
    if(name&&!strcmp(name,"AAudio_setMMapPolicy"))return (void*)klaa_setMMapPolicy;
    if(name&&!strcmp(name,"AAudioStreamBuilder_openStream"))return (void*)klaa_openWithPolicy;
    return kl_aaudio_base_lookup(name);
}
void *kl_aaudio_sym(const char *name){
    // dlsym is a capability query. An unknown optional export must remain
    // absent, rather than advertising a callable function that later aborts.
    return kl_aaudio_lookup(name);
}
