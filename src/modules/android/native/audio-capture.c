#include "audio-capture.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include "kl_audio.h"
#include "audio-hardware.h"
#define AAUDIO_FORMAT_PCM_FLOAT 2
#include KLEPTON_AUDIO_CAPTURE_STATE
static int klj_capture_read_pcm(KljAudioCapture *,int16_t *,int);
#include KLEPTON_AUDIO_CAPTURE_SOURCE
static pthread_mutex_t captureLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned users;
static int16_t *shared_pcm;
static size_t shared_capacity;
static uint64_t shared_written;
static unsigned shared_channels;

/* The original realtime producer still writes its original ring. Existing
 * reader threads drain it once into shared immutable PCM history; a consumer
 * moving its own cursor never consumes another stream's samples. */
static int klj_capture_read_pcm(KljAudioCapture *capture,int16_t *samples,int frames){
    if(!capture||!samples||frames<=0||!shared_capacity)return 0;
    size_t have=(size_t)(shared_written-capture->cursor);
    size_t wanted=(size_t)frames>have?(size_t)frames-have:0;
    if(wanted>shared_capacity-have)wanted=shared_capacity-have;
    while(wanted){
        size_t offset=(size_t)(shared_written%shared_capacity),piece=shared_capacity-offset;
        if(piece>wanted)piece=wanted;
        int count=kl_audio_mic_read_i16(shared_pcm+offset*shared_channels,(int)piece);
        if(count<0||(size_t)count>piece)return -896; // AAUDIO_ERROR_INTERNAL
        shared_written+=(unsigned)count;wanted-=(unsigned)count;
        if((size_t)count<piece)break;
    }
    have=(size_t)(shared_written-capture->cursor);
    size_t take=have<(size_t)frames?have:(size_t)frames;
    for(size_t done=0;done<take;){
        size_t offset=(size_t)((capture->cursor+done)%shared_capacity),piece=shared_capacity-offset;
        if(piece>take-done)piece=take-done;
        memcpy(samples+done*shared_channels,shared_pcm+offset*shared_channels,piece*shared_channels*sizeof(int16_t));done+=piece;
    }
    capture->cursor+=take;return (int)take;
}
KljAudioCapture *klj_audio_capture_create(unsigned rate,unsigned channels,int format){
    if(!rate||!channels||channels>8||(format!=1&&format!=2))return NULL;
    KljAudioCapture *capture=calloc(1,sizeof(*capture));if(!capture)return NULL;
    pthread_mutex_lock(&captureLock);
    if(!kl_audio_mic_enabled()||(!users&&kl_audio_mic_open(rate,channels))){pthread_mutex_unlock(&captureLock);free(capture);return NULL;}
    int32_t actual_rate,actual_channels,actual_format;
    if(klj_audio_capture_hardware_info(&actual_rate,&actual_channels,&actual_format)||actual_rate<1||actual_channels<1||actual_channels>8||actual_format!=1){
        if(!users)kl_audio_mic_close();pthread_mutex_unlock(&captureLock);free(capture);return NULL;
    }
    if(!users){
        shared_capacity=(size_t)actual_rate+1;shared_channels=(unsigned)actual_channels;shared_written=0;
        shared_pcm=calloc(shared_capacity*shared_channels,sizeof(*shared_pcm));
        if(!shared_pcm){shared_capacity=0;kl_audio_mic_close();pthread_mutex_unlock(&captureLock);free(capture);return NULL;}
    }
    ++users;capture->rate=rate;capture->channels=channels;capture->format=format;
    capture->mic_rate=(unsigned)actual_rate;capture->mic_ch=(unsigned)actual_channels;capture->cursor=shared_written;
    pthread_mutex_unlock(&captureLock);return capture;
}
void klj_audio_capture_destroy(KljAudioCapture *capture){
    if(!capture)return;
    pthread_mutex_lock(&captureLock);if(users&&!--users){kl_audio_mic_close();free(shared_pcm);shared_pcm=NULL;shared_capacity=0;shared_written=0;}pthread_mutex_unlock(&captureLock);
    free(capture->in_hold);free(capture);
}
int32_t klj_audio_capture_read(KljAudioCapture *capture,void *samples,int32_t frames,int64_t timeout){
    if(!capture||!samples)return -886; // AAUDIO_ERROR_NULL
    if(frames<0||timeout<0)return -898; // AAUDIO_ERROR_ILLEGAL_ARGUMENT
    if(!frames)return 0;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    int64_t start=(int64_t)now.tv_sec*1000000000LL+now.tv_nsec;
    int64_t deadline=timeout>INT64_MAX-start?INT64_MAX:start+timeout;
    int32_t produced=0;
    do{
        pthread_mutex_lock(&captureLock);
        int32_t rate,channels,format;
        if(!kl_audio_mic_enabled()||klj_audio_capture_hardware_info(&rate,&channels,&format)||
           rate!=(int32_t)capture->mic_rate||channels!=(int32_t)capture->mic_ch){
            capture->disconnected=1;
        }
        if(capture->disconnected){pthread_mutex_unlock(&captureLock);return produced?produced:-899;}
        uint64_t oldest=shared_written>shared_capacity?shared_written-shared_capacity:0;
        if(capture->cursor<oldest){
            capture->cursor=oldest;if(capture->overruns<UINT_MAX)capture->overruns++;
            capture->in_have=0;capture->in_pos=0;memset(capture->in_lp,0,sizeof capture->in_lp);
        }
        size_t bytes=capture->format==AAUDIO_FORMAT_PCM_FLOAT?sizeof(float):sizeof(int16_t);
        int32_t count=klj_capture_original_capture_into(capture,(char *)samples+(size_t)produced*capture->channels*bytes,frames-produced,0);
        pthread_mutex_unlock(&captureLock);
        if(count<0)return produced?produced:count;
        produced+=count;if(produced>=frames||!timeout)break;
        clock_gettime(CLOCK_MONOTONIC,&now);if((int64_t)now.tv_sec*1000000000LL+now.tv_nsec>=deadline)break;
        usleep(2000);
    }while(1);
    return produced;
}
unsigned klj_audio_capture_rate(const KljAudioCapture *capture){return capture?capture->mic_rate:0;}
unsigned klj_audio_capture_channels(const KljAudioCapture *capture){return capture?capture->mic_ch:0;}
unsigned klj_audio_capture_overruns(const KljAudioCapture *capture){pthread_mutex_lock(&captureLock);unsigned count=capture?capture->overruns:0;pthread_mutex_unlock(&captureLock);return count;}
