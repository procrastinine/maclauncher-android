// Owned output pause operation over the pinned upstream stream and sink.
// No guest structure is mirrored and no native stream is replaced.
static _Thread_local void *owned_feeding;
#include <stdbool.h>
#include <stdatomic.h>
#include "bionic/aaudio/callback-owner.h"
#include "audio-hardware.h"
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
    aaudio_result_t result=klaa_openStream(builder,output);
    if(result!=AAUDIO_OK)return result;
    klaa_stream *stream=*output;
    if(stream->direction==AAUDIO_DIRECTION_OUTPUT){
        stream->output_progress=klj_audio_source_create();
        if(!stream->output_progress){klaa_close(stream);*output=NULL;return AAUDIO_ERROR_NO_MEMORY;}
    }
    stream->content_type=builder->content_type;
    stream->usage=builder->usage;
    stream->capture_policy=builder->capture_policy;
    stream->content_spatialized=builder->content_spatialized;
    stream->privacy_sensitive=stream->direction==AAUDIO_DIRECTION_INPUT &&
        (builder->privacy_sensitive>=0?builder->privacy_sensitive:
        (stream->input_preset==5||stream->input_preset==7));
    memcpy(stream->owned_callbacks,builder->owned_callbacks,sizeof(stream->owned_callbacks));
    if(builder->data_user==&builder->owned_callbacks[0])stream->data_user=&stream->owned_callbacks[0];
    if(builder->error_user==&builder->owned_callbacks[1])stream->error_user=&stream->owned_callbacks[1];
    stream->presentation_cb=builder->presentation_cb;
    stream->presentation_user=builder->presentation_user==&builder->owned_callbacks[2]
        ?&stream->owned_callbacks[2]:builder->presentation_user;
    return result;
}

// The builder may be destroyed immediately after opening. Each original stream
// retains its own immutable descriptor until its feeder has joined on close.
static void owned_aaudio_callback_install(void *raw,unsigned kind,void *native,const KljAAudioCallback *callback){
    klaa_builder *builder=raw;
    if(!builder||kind<1||kind>3)return;
    KljAAudioCallback *owned=&builder->owned_callbacks[kind-1];
    *owned=callback?*callback:(KljAAudioCallback){0};
    if(kind==KLJ_AAUDIO_CALLBACK_DATA)klaa_setDataCallback(builder,(klaa_data_cb)native,native?owned:NULL);
    else if(kind==KLJ_AAUDIO_CALLBACK_ERROR)klaa_setErrorCallback(builder,(klaa_error_cb)native,native?owned:NULL);
    else {builder->presentation_cb=(void(*)(void*,void*))native;builder->presentation_user=native?owned:NULL;}
}
static void owned_set_presentation(klaa_builder *builder,void (*callback)(void*,void*),void *user){
    if(builder){builder->presentation_cb=callback;builder->presentation_user=user;}
}

static aaudio_result_t owned_aaudio_release(klaa_stream *stream){
    if(!stream)return AAUDIO_ERROR_NULL;
    pthread_mutex_lock(&stream->lock);
    if(owned_feeding==stream||(stream->thread_live&&pthread_equal(stream->thread,pthread_self()))){
        pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_INVALID_STATE;
    }
    if(stream->resources_released){pthread_mutex_unlock(&stream->lock);return AAUDIO_OK;}
    stream->resources_released=1;
    pthread_mutex_unlock(&stream->lock);
    klaa_stop_join(stream);
    if(stream->direction==AAUDIO_DIRECTION_INPUT){
        if(--g_input_streams<=0)g_input_streams=0;
        klj_audio_capture_destroy(stream->capture);stream->capture=NULL;
    }else{
        klj_audio_source_retire(stream->output_progress,stream);
        if(--g_open_streams<=0){g_open_streams=0;kl_audio_close();}
    }
    free(stream->scratch);stream->scratch=NULL;
    free(stream->staging);stream->staging=NULL;
    pthread_mutex_lock(&stream->lock);
    stream->state=AAUDIO_STREAM_STATE_CLOSING;
    pthread_mutex_unlock(&stream->lock);
    return AAUDIO_OK;
}

static aaudio_result_t owned_request_flush(klaa_stream *stream){
    if(!stream)return AAUDIO_ERROR_NULL;
    if(stream->direction!=AAUDIO_DIRECTION_OUTPUT)return AAUDIO_ERROR_UNIMPLEMENTED;
    pthread_mutex_lock(&stream->lock);
    int32_t state=stream->state;
    if(stream->resources_released||owned_feeding==stream||
       (state!=AAUDIO_STREAM_STATE_OPEN&&state!=AAUDIO_STREAM_STATE_PAUSED&&
        state!=AAUDIO_STREAM_STATE_STOPPED&&state!=AAUDIO_STREAM_STATE_FLUSHED)){
        pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_INVALID_STATE;
    }
    if(g_open_streams!=1){pthread_mutex_unlock(&stream->lock);return AAUDIO_ERROR_UNAVAILABLE;}
    int paused=kl_audio_pause_exclusive(stream);
    if(paused!=1){pthread_mutex_unlock(&stream->lock);return paused<0?AAUDIO_ERROR_INTERNAL:AAUDIO_ERROR_UNAVAILABLE;}
    stream->state=AAUDIO_STREAM_STATE_FLUSHING;
    kl_audio_flush();
    stream->state=AAUDIO_STREAM_STATE_FLUSHED;
    pthread_mutex_unlock(&stream->lock);
    return AAUDIO_OK;
}

static int32_t owned_get_usage(klaa_stream *stream){return stream?stream->usage:AAUDIO_ERROR_NULL;}
static int32_t owned_get_content(klaa_stream *stream){return stream?stream->content_type:AAUDIO_ERROR_NULL;}
static int32_t owned_get_capture_policy(klaa_stream *stream){return stream?stream->capture_policy:AAUDIO_ERROR_NULL;}
// Output frame accounting follows https://github.com/shinyquagsire23/Klepton/pull/12
// (3dde04e7b33039a3d041590b44aaa2c81cd3f584). Each stream observes its own
// committed mixer progress independently of producer pacing and other streams.
static int64_t owned_get_frames_read(klaa_stream *stream){
    if(!stream)return 0;
    if(stream->direction==AAUDIO_DIRECTION_INPUT)return klaa_getFramesRead(stream);
    return (int64_t)klj_audio_source_frames_read(stream->output_progress,atomic_load_explicit(&stream->frames,memory_order_acquire));
}
static bool owned_get_spatialized(klaa_stream *stream){return stream&&stream->content_spatialized;}
static bool owned_get_privacy(klaa_stream *stream){return stream&&stream->privacy_sensitive;}
static int32_t owned_get_hardware(klaa_stream *stream,unsigned which){
    if(!stream)return AAUDIO_ERROR_NULL;
    if(stream->resources_released)return AAUDIO_ERROR_INVALID_STATE;
    int32_t rate,channels,format;
    int result=stream->direction==AAUDIO_DIRECTION_INPUT
        ?klj_audio_capture_hardware_info(&rate,&channels,&format)
        :klj_audio_hardware_info(&rate,&channels,&format);
    return result?result:(which==0?rate:which==1?channels:format);
}
static int32_t owned_get_hardware_rate(klaa_stream *s){return owned_get_hardware(s,0);}
static int32_t owned_get_hardware_channels(klaa_stream *s){return owned_get_hardware(s,1);}
static int32_t owned_get_hardware_format(klaa_stream *s){return owned_get_hardware(s,2);}
static int32_t owned_get_xruns(klaa_stream *stream){
    if(!stream)return AAUDIO_ERROR_NULL;
    if(stream->direction!=AAUDIO_DIRECTION_INPUT)return klaa_getXRunCount(stream);
    if(stream->resources_released)return AAUDIO_ERROR_INVALID_STATE;
    unsigned count=klj_audio_capture_overruns(stream->capture);return count>INT32_MAX?INT32_MAX:(int32_t)count;
}
static aaudio_result_t owned_get_timestamp(klaa_stream *stream,clockid_t clock,int64_t *position,int64_t *nanos){
    if(!stream||!position||!nanos)return AAUDIO_ERROR_NULL;
    // Linux clock IDs are carried unchanged by the guest ABI. A native caller
    // may also pass Darwin's CLOCK_MONOTONIC. No sink presentation timestamp is
    // exposed by the original backend, so preserve output values on absence.
    if(clock!=1&&clock!=7&&clock!=CLOCK_MONOTONIC)return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    if(klaa_getState(stream)!=AAUDIO_STREAM_STATE_STARTED)return AAUDIO_ERROR_INVALID_STATE;
    return AAUDIO_ERROR_UNIMPLEMENTED;
}
static int32_t owned_offload_result(klaa_stream *stream){
    if(!stream)return AAUDIO_ERROR_NULL;
    return stream->resources_released?AAUDIO_ERROR_INVALID_STATE:AAUDIO_ERROR_UNIMPLEMENTED;
}
static aaudio_result_t owned_offload_delay_padding(klaa_stream *stream,int32_t delay,int32_t padding){
    if(!stream)return AAUDIO_ERROR_NULL;
    if(delay<0||padding<0)return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    return owned_offload_result(stream);
}
static const char *owned_state_text(int32_t state){
    static const char *const states[]={"Uninitialized","Unknown","Open","Starting","Started","Pausing","Paused","Flushing","Flushed","Stopping","Stopped","Closing","Closed","Disconnected"};
    return state>=0&&state<(int32_t)(sizeof(states)/sizeof(states[0]))?states[state]:"Unrecognized";
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
    if(!name)return NULL;
#define OWNED(name_,function) if(!strcmp(name,name_))return (void*)function
    OWNED("klj_aaudio_callback_owner_v1",owned_aaudio_callback_install);
    OWNED("AAudioStreamBuilder_setPresentationEndCallback",owned_set_presentation);
    OWNED("AAudioStreamBuilder_setSamplesPerFrame",klaa_setChannelCount);
    OWNED("AAudioStream_getAllowedCapturePolicy",owned_get_capture_policy);
    OWNED("AAudioStream_getContentType",owned_get_content);
    OWNED("AAudioStream_getFramesRead",owned_get_frames_read);
    OWNED("AAudioStream_getHardwareChannelCount",owned_get_hardware_channels);
    OWNED("AAudioStream_getHardwareFormat",owned_get_hardware_format);
    OWNED("AAudioStream_getHardwareSampleRate",owned_get_hardware_rate);
    OWNED("AAudioStream_getXRunCount",owned_get_xruns);
    OWNED("AAudioStream_getOffloadDelay",owned_offload_result);
    OWNED("AAudioStream_getOffloadPadding",owned_offload_result);
    OWNED("AAudioStream_getTimestamp",owned_get_timestamp);
    OWNED("AAudioStream_getUsage",owned_get_usage);
    OWNED("AAudioStream_isContentSpatialized",owned_get_spatialized);
    OWNED("AAudioStream_isPrivacySensitive",owned_get_privacy);
    OWNED("AAudioStream_release",owned_aaudio_release);
    OWNED("AAudioStream_requestFlush",owned_request_flush);
    OWNED("AAudioStream_setOffloadDelayPadding",owned_offload_delay_padding);
    OWNED("AAudioStream_setOffloadEndOfStream",owned_offload_result);
    OWNED("AAudio_convertStreamStateToText",owned_state_text);
#undef OWNED
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
