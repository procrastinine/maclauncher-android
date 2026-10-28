// Owned adaptation: pause the real sole producer without dropping its queued PCM.
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <AudioToolbox/AudioToolbox.h>
#include "audio-volume.h"
#include "audio-pcm.h"
#include "audio-hardware.h"
/* SOURCE_PROGRESS_STATE_BEGIN */
struct KljAudioSourceProgress {
    _Atomic uint64_t sequence,epoch,committed,end,source_frames,device_frames,seen,carried;
    _Atomic int retired;
    struct KljAudioSourceProgress *next;
};
_Static_assert(ATOMIC_LONG_LOCK_FREE==2&&ATOMIC_LLONG_LOCK_FREE==2,"Audio progress requires lock-free native counters");
static _Atomic uint64_t owned_audio_epoch=1;
static KljAudioSourceProgress *owned_audio_progress;
struct OwnedAudioWrite {
    KljAudioSourceProgress *progress;
    uint64_t frames,base,epoch;
    int initialized;
};
static _Thread_local struct OwnedAudioWrite *owned_audio_write;
static void owned_audio_commit(uint64_t end,uint64_t written,uint64_t total);
static void owned_audio_reset_progress(void);
/* SOURCE_PROGRESS_STATE_END */
static _Atomic int owned_paused;
static _Atomic uint32_t owned_capture_device;
static pthread_mutex_t owned_sources_lock=PTHREAD_MUTEX_INITIALIZER;
static const void *owned_source;
static int owned_source_seen,owned_source_mixed;
static _Atomic int owned_watchdog_running,owned_recovery_pending;
static pthread_mutex_t owned_recovery_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int owned_recovery_writer;
static int owned_recovery_begin(uint64_t limit);
static void owned_recovery_end(void);
static OSStatus owned_render(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32,AudioBufferList*);
static void owned_note_source(const void *source){
    pthread_mutex_lock(&owned_sources_lock);
    if(!owned_source_seen){owned_source=source;owned_source_seen=1;}
    else if(owned_source!=source){
        owned_source_mixed=1;
        // A later independent producer must never inherit another stream's mute.
        atomic_store_explicit(&owned_paused,0,memory_order_release);
    }
    pthread_mutex_unlock(&owned_sources_lock);
}
#define render_cb owned_original_render
#define kl_audio_play owned_original_play
#define kl_audio_open owned_original_open
#define kl_audio_close owned_original_close
#define kl_audio_write_src owned_original_write_src
#define kl_audio_mic_read_i16 owned_original_mic_read_i16
#include KLEPTON_AUDIO_PAUSE_SOURCE
#undef render_cb
#undef kl_audio_play
#undef kl_audio_open
#undef kl_audio_close
#undef kl_audio_write_src
#undef kl_audio_mic_read_i16

// Only a stale device needs lifecycle locking. Re-read its heartbeat after
// acquiring producer ownership: another recovery or a resumed callback can
// already have made the original observation obsolete.
static int owned_recovery_begin(uint64_t limit){
    if(pthread_mutex_trylock(&owned_recovery_lock)!=0)return 0;
    if(!owned_recovery_writer){
        atomic_store_explicit(&owned_recovery_pending,1,memory_order_release);
        pthread_mutex_lock(&g_wlock);
    }
    uint64_t now=now_ns(),last=atomic_load_explicit(&g_last_render_ns,memory_order_acquire);
    if(!g_open||!g_playing||g_interrupted||now<=last||now-last<=limit){owned_recovery_end();return 0;}
    return 1;
}
static void owned_recovery_end(void){
    if(!owned_recovery_writer){
        pthread_mutex_unlock(&g_wlock);
        atomic_store_explicit(&owned_recovery_pending,0,memory_order_release);
    }
    pthread_mutex_unlock(&owned_recovery_lock);
}

// A single selected input serves Java, AAudio and OpenSL through Klepton's
// existing capture ring. No default-input fallback or second capture unit.
void klj_audio_capture_select(uint32_t device){
    kl_audio_mic_set_enabled(0);
    atomic_store_explicit(&owned_capture_device,device,memory_order_release);
    if(device)kl_audio_mic_set_enabled(1);
}
int kl_audio_mic_read_i16(int16_t *samples,int frames){
    pthread_mutex_lock(&g_mic_lock);
    int result=owned_original_mic_read_i16(samples,frames);
    pthread_mutex_unlock(&g_mic_lock);
    return result;
}

// build-aaudio replaces the one AudioUnit callback assignment with owned_render.
// The realtime callback only reads one atomic; it never takes the source lock.
static OSStatus owned_render(void *ref,AudioUnitRenderActionFlags *flags,const AudioTimeStamp *ts,UInt32 bus,UInt32 n,AudioBufferList *io){
    if(atomic_load_explicit(&owned_paused,memory_order_acquire)){
        if(io)for(UInt32 i=0;i<io->mNumberBuffers;i++)if(io->mBuffers[i].mData)memset(io->mBuffers[i].mData,0,io->mBuffers[i].mDataByteSize);
        if(flags)*flags|=kAudioUnitRenderAction_OutputIsSilence;
        atomic_store_explicit(&g_last_render_ns,now_ns(),memory_order_relaxed);
    }
    OSStatus result=atomic_load_explicit(&owned_paused,memory_order_acquire)?noErr:owned_original_render(ref,flags,ts,bus,n,io);
    if(result==noErr&&io){
        for(UInt32 i=0;i<io->mNumberBuffers;i++)klj_audio_pcm_mix(io->mBuffers[i].mData,io->mBuffers[i].mDataByteSize/(sizeof(float)*g_out_ch),g_out_ch,g_out_rate);
        // Klepton emits interleaved float PCM. Scale the consumed output, not
        // the source ring: mute still advances playback and queued data stays intact.
        for(UInt32 i=0;i<io->mNumberBuffers;i++){
            klj_audio_scale_pcm(io->mBuffers[i].mData,io->mBuffers[i].mDataByteSize/sizeof(float),3);
            klj_audio_filter_output(io->mBuffers[i].mData,io->mBuffers[i].mDataByteSize/sizeof(float));
        }
    }
    return result;
}
int kl_audio_pause_exclusive(const void *source){
    pthread_mutex_lock(&owned_sources_lock);
    if(klj_audio_pcm_has_sources()||owned_source_mixed||(owned_source_seen&&owned_source!=source)){
        pthread_mutex_unlock(&owned_sources_lock);return 0;
    }
    owned_source=source;owned_source_seen=1;
    atomic_store_explicit(&owned_paused,1,memory_order_release);
    int was_playing=g_playing;
    kl_audio_pause();
    pthread_mutex_lock(&g_lock);
    OSStatus stopped=noErr;
    if(g_unit&&g_running){stopped=AudioOutputUnitStop(g_unit);if(stopped==noErr)g_running=0;}
    pthread_mutex_unlock(&g_lock);
    if(stopped!=noErr){g_playing=was_playing;atomic_store_explicit(&owned_paused,0,memory_order_release);}
    pthread_mutex_unlock(&owned_sources_lock);
    return stopped==noErr?1:-1;
}
void kl_audio_play(void){atomic_store_explicit(&owned_paused,0,memory_order_release);owned_original_play();}
size_t kl_audio_write_src(const void *source,const void *pcm,size_t bytes){return owned_original_write_src(source,pcm,bytes);}
int kl_audio_open(unsigned rate,unsigned channels,unsigned bits){
    pthread_mutex_lock(&g_wlock);
    int result=owned_original_open(rate,channels,bits);
    pthread_mutex_unlock(&g_wlock);return result;
}
void kl_audio_close(void){
    pthread_mutex_lock(&g_wlock);
    owned_original_close();
    pthread_mutex_unlock(&g_wlock);
    pthread_mutex_lock(&owned_sources_lock);
    owned_source=NULL;owned_source_seen=owned_source_mixed=0;
    atomic_store_explicit(&owned_paused,0,memory_order_release);
    pthread_mutex_unlock(&owned_sources_lock);
}

// Java PCM owns its format and queue, mixing into this existing device callback.
int klj_audio_pcm_device(void){
    pthread_mutex_lock(&g_lock);int opened=g_open;pthread_mutex_unlock(&g_lock);
    if(!opened&&kl_audio_open(48000,2,16)!=0)return -1;
    kl_audio_play();return 0;
}

// Report the PCM format adopted by the actual native AudioUnit owner. These
// queries do not open a device and never replay a guest stream's requested
// rate, channel count or sample format.
int klj_audio_hardware_info(int32_t *rate,int32_t *channels,int32_t *format){
    if(!rate||!channels||!format)return -886; // AAUDIO_ERROR_NULL
    pthread_mutex_lock(&g_lock);
    int available=g_open&&g_unit&&g_out_rate>=1.0&&g_out_rate<=INT32_MAX&&g_out_ch>0&&g_out_ch<=INT32_MAX;
    if(available){*rate=(int32_t)g_out_rate;*channels=(int32_t)g_out_ch;*format=2;} // native float PCM
    pthread_mutex_unlock(&g_lock);
    return available?0:-889; // AAUDIO_ERROR_UNAVAILABLE
}
int klj_audio_capture_hardware_info(int32_t *rate,int32_t *channels,int32_t *format){
    if(!rate||!channels||!format)return -886;
    pthread_mutex_lock(&g_mic_lock);
    int available=g_mic_open&&g_cap_unit&&g_mic_rate>0&&g_mic_rate<=INT32_MAX&&g_mic_ch>0&&g_mic_ch<=INT32_MAX;
    if(available){*rate=(int32_t)g_mic_rate;*channels=(int32_t)g_mic_ch;*format=1;} // native capture int16 PCM
    pthread_mutex_unlock(&g_mic_lock);
    return available?0:-889;
}

/* SOURCE_PROGRESS_OPERATIONS_BEGIN */
KljAudioSourceProgress *klj_audio_source_create(void){
    KljAudioSourceProgress *value=malloc(sizeof(*value));
    if(!value)return NULL;
    atomic_init(&value->sequence,0);atomic_init(&value->epoch,0);
    atomic_init(&value->committed,0);atomic_init(&value->end,0);
    atomic_init(&value->source_frames,0);atomic_init(&value->device_frames,0);
    atomic_init(&value->seen,0);atomic_init(&value->carried,0);atomic_init(&value->retired,0);
    pthread_mutex_lock(&g_wlock);value->next=owned_audio_progress;owned_audio_progress=value;pthread_mutex_unlock(&g_wlock);
    return value;
}

static uint64_t owned_audio_consumed(uint64_t committed,uint64_t end,uint64_t source,uint64_t device,uint64_t read){
    if(!device)return 0;
    __uint128_t pending=end>read?((__uint128_t)(end-read)*source+device-1)/device:0;
    return pending>=committed?0:committed-(uint64_t)pending;
}

// Device open/close holds g_wlock before the original lifecycle lock. The old
// AudioUnit has stopped here; preserve consumption even if nobody queried it.
static void owned_audio_reset_progress(void){
    uint64_t epoch=atomic_load_explicit(&owned_audio_epoch,memory_order_relaxed);
    uint64_t read=atomic_load_explicit(&g_r,memory_order_acquire);
    for(KljAudioSourceProgress *value=owned_audio_progress;value;value=value->next){
        if(atomic_load_explicit(&value->epoch,memory_order_relaxed)!=epoch)continue;
        uint64_t consumed=owned_audio_consumed(atomic_load_explicit(&value->committed,memory_order_relaxed),
            atomic_load_explicit(&value->end,memory_order_relaxed),atomic_load_explicit(&value->source_frames,memory_order_relaxed),
            atomic_load_explicit(&value->device_frames,memory_order_relaxed),read);
        uint64_t seen=atomic_load_explicit(&value->seen,memory_order_acquire);
        atomic_store_explicit(&value->carried,consumed>seen?consumed:seen,memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&owned_audio_epoch,1,memory_order_acq_rel);
}

// The original writer holds g_wlock here, only after copying/mixing this range.
// The cumulative count excludes a callback's not-yet-written next burst.
static void owned_audio_commit(uint64_t end,uint64_t written,uint64_t total){
    struct OwnedAudioWrite *write=owned_audio_write;
    if(!write||!total||atomic_load_explicit(&write->progress->retired,memory_order_acquire))return;
    KljAudioSourceProgress *value=write->progress;
    uint64_t epoch=atomic_load_explicit(&owned_audio_epoch,memory_order_acquire);
    if(!write->initialized||write->epoch!=epoch){
        write->base=atomic_load_explicit(&value->epoch,memory_order_relaxed)==epoch
            ?atomic_load_explicit(&value->committed,memory_order_relaxed)
            :atomic_load_explicit(&value->carried,memory_order_relaxed);
        write->epoch=epoch;write->initialized=1;
    }
    __uint128_t count=(__uint128_t)write->frames*written/total+write->base;
    uint64_t committed=count>INT64_MAX?INT64_MAX:(uint64_t)count;
    atomic_fetch_add_explicit(&value->sequence,1,memory_order_acq_rel);
    atomic_store_explicit(&value->epoch,epoch,memory_order_relaxed);
    atomic_store_explicit(&value->committed,committed,memory_order_relaxed);
    atomic_store_explicit(&value->end,end,memory_order_relaxed);
    atomic_store_explicit(&value->source_frames,write->frames,memory_order_relaxed);
    atomic_store_explicit(&value->device_frames,total,memory_order_relaxed);
    atomic_fetch_add_explicit(&value->sequence,1,memory_order_release);
}

size_t klj_audio_source_write(KljAudioSourceProgress *progress,const void *source,const void *pcm,size_t bytes,uint64_t frames){
    if(!progress||!frames)return 0;
    struct OwnedAudioWrite write={.progress=progress,.frames=frames};
    struct OwnedAudioWrite *previous=owned_audio_write;owned_audio_write=&write;
    size_t result=owned_original_write_src(source,pcm,bytes);
    owned_audio_write=previous;return result;
}

uint64_t klj_audio_source_frames_read(KljAudioSourceProgress *value,uint64_t maximum){
    if(!value)return 0;
    uint64_t previous=atomic_load_explicit(&value->seen,memory_order_acquire);
    if(atomic_load_explicit(&value->retired,memory_order_acquire))return previous;
    uint64_t epoch=atomic_load_explicit(&owned_audio_epoch,memory_order_acquire);
    uint64_t sequence=atomic_load_explicit(&value->sequence,memory_order_acquire);
    if(sequence&1)return previous;
    uint64_t committed=atomic_load_explicit(&value->committed,memory_order_relaxed);
    uint64_t end=atomic_load_explicit(&value->end,memory_order_relaxed);
    uint64_t source=atomic_load_explicit(&value->source_frames,memory_order_relaxed);
    uint64_t device=atomic_load_explicit(&value->device_frames,memory_order_relaxed);
    uint64_t producedEpoch=atomic_load_explicit(&value->epoch,memory_order_relaxed);
    uint64_t read=atomic_load_explicit(&g_r,memory_order_acquire);
    atomic_thread_fence(memory_order_acquire);
    // Publication overlap returns the last observation; a retired device epoch
    // supplies its final consumed count without waiting for a paced writer.
    if(sequence!=atomic_load_explicit(&value->sequence,memory_order_acquire))return previous;
    uint64_t consumed;
    if(epoch!=atomic_load_explicit(&owned_audio_epoch,memory_order_acquire)||producedEpoch!=epoch)
        consumed=atomic_load_explicit(&value->carried,memory_order_acquire);
    else consumed=owned_audio_consumed(committed,end,source,device,read);
    if(consumed>maximum)consumed=maximum;
    while(previous<consumed&&!atomic_compare_exchange_weak_explicit(&value->seen,&previous,consumed,memory_order_release,memory_order_acquire)){}
    return previous>consumed?previous:consumed;
}

void klj_audio_source_retire(KljAudioSourceProgress *value,const void *source){
    if(!value)return;
    // The feeder has joined. Retire the exact key while writers are serialized,
    // so another stream allocated at this address starts with a new mixer slot.
    pthread_mutex_lock(&g_wlock);
    atomic_store_explicit(&value->retired,1,memory_order_release);
    KljAudioSourceProgress **at=&owned_audio_progress;
    while(*at&&*at!=value)at=&(*at)->next;
    if(*at)*at=value->next;
    for(int i=0;i<g_src_n;i++)if(g_src_key[i]==source){
        for(int next=i+1;next<g_src_n;next++){g_src_key[next-1]=g_src_key[next];g_src_pos[next-1]=g_src_pos[next];}
        g_src_n--;g_src_key[g_src_n]=NULL;g_src_pos[g_src_n]=0;break;
    }
    pthread_mutex_unlock(&g_wlock);
}
void klj_audio_source_destroy(KljAudioSourceProgress *value){free(value);}
/* SOURCE_PROGRESS_OPERATIONS_END */
