#include "audio-capture.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "kl_audio.h"
#define AAUDIO_FORMAT_PCM_FLOAT 2
#include KLEPTON_AUDIO_CAPTURE_STATE
#include KLEPTON_AUDIO_CAPTURE_SOURCE
static pthread_mutex_t captureLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned users;
KljAudioCapture *klj_audio_capture_create(unsigned rate,unsigned channels,int format){
    if(!rate||!channels||channels>8||(format!=1&&format!=2))return NULL;
    KljAudioCapture *capture=calloc(1,sizeof(*capture));if(!capture)return NULL;
    pthread_mutex_lock(&captureLock);
    if(kl_audio_mic_open(rate,channels)){pthread_mutex_unlock(&captureLock);free(capture);return NULL;}
    ++users;capture->rate=rate;capture->channels=channels;capture->format=format;
    capture->mic_rate=kl_audio_mic_rate();capture->mic_ch=kl_audio_mic_channels();
    pthread_mutex_unlock(&captureLock);return capture;
}
void klj_audio_capture_destroy(KljAudioCapture *capture){
    if(!capture)return;
    pthread_mutex_lock(&captureLock);if(users&&!--users)kl_audio_mic_close();pthread_mutex_unlock(&captureLock);
    free(capture->in_hold);free(capture);
}
int32_t klj_audio_capture_read(KljAudioCapture *capture,void *samples,int32_t frames,int64_t timeout){return klj_capture_original_capture_into(capture,samples,frames,timeout);}
unsigned klj_audio_capture_rate(const KljAudioCapture *capture){return capture?capture->mic_rate:0;}
unsigned klj_audio_capture_channels(const KljAudioCapture *capture){return capture?capture->mic_ch:0;}
