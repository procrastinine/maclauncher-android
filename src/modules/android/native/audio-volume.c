#include "audio-volume.h"
#include <stdatomic.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
/* Integer atomics are lock-free on the supported Mac architecture. The audio
 * callback allocates nothing, locks nothing and never calls Java. */
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"Audio volume requires lock-free integer atomics");
static _Atomic int volumes[KLJ_AUDIO_STREAMS]={15,15,15,15,15,15,15,15,15,15,15,15};
static _Atomic unsigned active[KLJ_AUDIO_STREAMS];
static _Atomic uint64_t stopped[KLJ_AUDIO_STREAMS];
static _Atomic(KljAudioOutputFilter) output_filter;
void klj_audio_set_output_filter(KljAudioOutputFilter filter){atomic_store_explicit(&output_filter,filter,memory_order_release);}
void klj_audio_filter_output(float *samples,size_t count){
    KljAudioOutputFilter filter=atomic_load_explicit(&output_filter,memory_order_acquire);
    if(filter&&samples&&count)filter(samples,count);
}
static uint64_t activity_millis(void){struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;}
void klj_audio_activity_transition(int stream,int was_playing,int playing){
    if(stream<0||stream>=KLJ_AUDIO_STREAMS||!!was_playing==!!playing)return;
    if(playing)atomic_fetch_add_explicit(&active[stream],1,memory_order_release);
    else{atomic_store_explicit(&stopped[stream],activity_millis(),memory_order_release);atomic_fetch_sub_explicit(&active[stream],1,memory_order_release);}
}
int klj_audio_stream_active(int stream,int in_past_ms){
    if(stream<0||stream>=KLJ_AUDIO_STREAMS)return 0;
    if(atomic_load_explicit(&active[stream],memory_order_acquire))return 1;
    uint64_t last=atomic_load_explicit(&stopped[stream],memory_order_acquire),now=activity_millis();
    return in_past_ms>0&&last&&now>=last&&now-last<(uint64_t)in_past_ms;
}
int klj_audio_set_stream_volume(int stream,int index){
    if(stream<0||stream>=KLJ_AUDIO_STREAMS||index<0||index>15)return 0;
    atomic_store_explicit(&volumes[stream],index,memory_order_relaxed);return 1;
}
float klj_audio_stream_gain(int stream){
    return stream<0||stream>=KLJ_AUDIO_STREAMS?0:atomic_load_explicit(&volumes[stream],memory_order_relaxed)/15.0f;
}
void klj_audio_scale_pcm(float *samples,size_t count,int stream){
    if(!samples)return;float gain=klj_audio_stream_gain(stream);
    if(gain==1)return;
    if(gain==0){memset(samples,0,count*sizeof(float));return;}
    for(size_t i=0;i<count;i++)samples[i]*=gain;
}
