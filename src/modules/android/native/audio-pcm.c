// PCM sources in Klepton's existing CoreAudio callback. Each stream retains its
// own queue/clock; AAudio/OpenSL input format changes cannot reinterpret it.
#include "audio-pcm.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <math.h>
#include <stdbool.h>
#include <limits.h>
typedef struct {
    float *ring;size_t capacity;unsigned channels;
    const int16_t *clip;_Atomic int loops;
    _Atomic double rate;
    _Atomic float left,right;
    _Atomic int dualMono;
    _Atomic uint64_t read,write,position,generation;
    _Atomic unsigned underruns;
    _Atomic bool playing;
    pthread_mutex_t producer;
    klj_audio_pcm_gain gain;void *gainOwner;uint64_t gainPosition;
    // Only the callback touches these, or control after pausing and draining it.
    double fraction;
} source;
typedef struct {size_t count;_Atomic(source*) items[];} snapshot;
static _Atomic(snapshot*) current;
static _Atomic unsigned readers;
static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static void quiesce(void){while(atomic_load_explicit(&readers,memory_order_seq_cst))sched_yield();}
static int publish(source *item,bool add){
    pthread_mutex_lock(&registry);snapshot *old=atomic_load(&current);size_t count=old?old->count:0;
    if(!add){
        bool any=false;
        for(size_t i=0;i<count;i++){source *entry=atomic_load(&old->items[i]);if(entry==item)atomic_store_explicit(&old->items[i],NULL,memory_order_seq_cst);else if(entry)any=true;}
        if(!any)atomic_store_explicit(&current,NULL,memory_order_seq_cst);
        quiesce();if(!any)free(old);pthread_mutex_unlock(&registry);return 0;
    }
    if(count>(SIZE_MAX-sizeof(snapshot))/sizeof(source*)-1){pthread_mutex_unlock(&registry);return -1;}
    snapshot *next=calloc(1,sizeof(*next)+(count+1)*sizeof(source*));if(!next){pthread_mutex_unlock(&registry);return -1;}
    for(size_t i=0;i<count;i++){source *entry=atomic_load(&old->items[i]);if(entry)atomic_store(&next->items[next->count++],entry);}
    atomic_store(&next->items[next->count++],item);atomic_store_explicit(&current,next,memory_order_seq_cst);
    quiesce();free(old);pthread_mutex_unlock(&registry);return 0;
}
int klj_audio_pcm_has_sources(void){return atomic_load(&current)!=NULL;}
void *klj_audio_pcm_create(size_t capacity,unsigned channels,double rate){return klj_audio_pcm_create_shaped(capacity,channels,rate,NULL,NULL);}
static void *create(size_t capacity,unsigned channels,double rate,klj_audio_pcm_gain gain,void *owner,const int16_t *clip){
    if(!capacity||!channels||capacity>SIZE_MAX/channels/sizeof(float)||!isfinite(rate)||rate<=0||klj_audio_pcm_device()!=0)return NULL;
    source *s=calloc(1,sizeof(*s));if(!s)return NULL;s->clip=clip;
    if(!clip){s->ring=calloc(capacity*channels,sizeof(float));if(!s->ring){free(s);return NULL;}}
    s->capacity=capacity;s->channels=channels;s->gain=gain;s->gainOwner=owner;atomic_store(&s->rate,rate);atomic_store(&s->left,1);atomic_store(&s->right,1);pthread_mutex_init(&s->producer,NULL);if(publish(s,true)!=0){pthread_mutex_destroy(&s->producer);free(s->ring);free(s);return NULL;}return s;
}
void *klj_audio_pcm_create_shaped(size_t capacity,unsigned channels,double rate,klj_audio_pcm_gain gain,void *owner){return create(capacity,channels,rate,gain,owner,NULL);}
void *klj_audio_pcm_clip(const int16_t *samples,size_t frames,unsigned channels,double rate){
    if(!samples)return NULL;source *s=create(frames,channels,rate,NULL,NULL,samples);
    if(s)atomic_store(&s->write,frames);return s;
}
static uint64_t clip_end(source *s){int loops=atomic_load(&s->loops);return loops<0||s->capacity>UINT64_MAX/((uint64_t)loops+1)?UINT64_MAX:s->capacity*((uint64_t)loops+1);}
void klj_audio_pcm_clip_loops(void *value,int loops){
    source *s=value;if(!s||!s->clip||loops< -1||atomic_load(&s->loops)==loops)return;
    bool playing=atomic_exchange_explicit(&s->playing,false,memory_order_seq_cst);pthread_mutex_lock(&s->producer);quiesce();
    // A loop-count change applies from the current traversal. In particular,
    // ending an infinite loop still renders the rest of its current sample.
    uint64_t read=atomic_load(&s->read);if(playing)read%=s->capacity;
    atomic_store(&s->read,read);atomic_store(&s->position,read);atomic_store(&s->loops,loops);atomic_store(&s->write,clip_end(s));
    atomic_store_explicit(&s->playing,playing,memory_order_seq_cst);pthread_mutex_unlock(&s->producer);
}
void klj_audio_pcm_clip_seek(void *value,uint64_t frame){
    source *s=value;if(!s||!s->clip)return;
    bool playing=atomic_exchange_explicit(&s->playing,false,memory_order_seq_cst);pthread_mutex_lock(&s->producer);quiesce();
    if(frame>s->capacity)frame=s->capacity;
    atomic_store(&s->read,frame);atomic_store(&s->position,frame);s->fraction=0;
    atomic_store_explicit(&s->playing,playing,memory_order_seq_cst);pthread_mutex_unlock(&s->producer);
}
int klj_audio_pcm_playing(void *value){source *s=value;return s&&atomic_load_explicit(&s->playing,memory_order_seq_cst);}
void klj_audio_pcm_controls(void *value,double rate,float left,float right,int dualMono){
    source *s=value;if(!s||!isfinite(rate)||rate<0)return;
    atomic_store(&s->rate,rate);atomic_store(&s->left,left);atomic_store(&s->right,right);atomic_store(&s->dualMono,dualMono);
}
void klj_audio_pcm_state(void *value,int state){
    source *s=value;if(!s)return;
    // Cancel a currently paced write before taking its producer mutex.
    atomic_store_explicit(&s->playing,false,memory_order_seq_cst);atomic_fetch_add(&s->generation,1);
    pthread_mutex_lock(&s->producer);quiesce();
    if(state==2||state==3){atomic_store(&s->read,0);atomic_store(&s->write,s->clip?clip_end(s):0);atomic_store(&s->position,0);s->fraction=0;}
    if(state==0&&s->clip&&atomic_load(&s->read)>=atomic_load(&s->write)){atomic_store(&s->read,0);atomic_store(&s->position,0);s->fraction=0;}
    if(state==0){klj_audio_pcm_device();atomic_store_explicit(&s->playing,true,memory_order_seq_cst);}
    pthread_mutex_unlock(&s->producer);
}
void klj_audio_pcm_destroy(void *value){source *s=value;if(!s)return;klj_audio_pcm_state(s,3);if(publish(s,false)!=0){return;}pthread_mutex_destroy(&s->producer);free(s->ring);free(s);}
int klj_audio_pcm_write(void *value,const float *pcm,int frames){
    source *s=value;if(!s||s->clip||frames<0||(!pcm&&frames))return -2;
    uint64_t generation=atomic_load(&s->generation);int done=0;pthread_mutex_lock(&s->producer);
    while(done<frames&&atomic_load(&s->generation)==generation){
        uint64_t w=atomic_load_explicit(&s->write,memory_order_relaxed),r=atomic_load_explicit(&s->read,memory_order_acquire);size_t free=s->capacity-(size_t)(w-r);
        if(!free){if(!atomic_load(&s->playing))break;usleep(1000);continue;}
        size_t take=(size_t)(frames-done)<free?(size_t)(frames-done):free;
        size_t at=(size_t)(w%s->capacity),first=take<s->capacity-at?take:s->capacity-at;
        memcpy(s->ring+at*s->channels,pcm+(size_t)done*s->channels,first*s->channels*sizeof(float));
        if(first<take)memcpy(s->ring,pcm+((size_t)done+first)*s->channels,(take-first)*s->channels*sizeof(float));
        atomic_store_explicit(&s->write,w+take,memory_order_release);done+=(int)take;
    }
    pthread_mutex_unlock(&s->producer);return done;
}
uint64_t klj_audio_pcm_position(void *value){source *s=value;return s?atomic_load(&s->position):0;}
uint64_t klj_audio_pcm_queued(void *value){source *s=value;if(!s)return 0;uint64_t w=atomic_load(&s->write),r=atomic_load(&s->read);return w>r?w-r:0;}
unsigned klj_audio_pcm_underruns(void *value){source *s=value;return s?atomic_load(&s->underruns):0;}
// Sampling and channel conversion happen once, here at the actual device rate.
// The queue retains all source channels and float precision; controls also apply
// to frames already queued, just as they do for a native AudioTrack.
static float sample(const source *s,uint64_t frameIndex,unsigned channel,double fraction,uint64_t end){
    if(s->clip){float a=s->clip[(frameIndex%s->capacity)*s->channels+channel]/32768.f;float b=s->clip[((frameIndex+1<end?frameIndex+1:frameIndex)%s->capacity)*s->channels+channel]/32768.f;return a+(b-a)*(float)fraction;}
    float a=s->ring[(frameIndex%s->capacity)*s->channels+channel];
    float b=s->ring[((frameIndex+1<end?frameIndex+1:frameIndex)%s->capacity)*s->channels+channel];
    return a+(b-a)*(float)fraction;
}
static float finite_sample(float value){return isfinite(value)?fmaxf(-1,fminf(1,value)):0;}
static void advance(source *s,uint64_t *read,uint64_t end){
    if(*read>=end){s->fraction=0;return;}
    // Roundoff at exact integral frame boundaries must not leave one frame
    // indefinitely queued (for example 441 source frames over 480 outputs).
    double whole=floor(s->fraction+1e-12);
    uint64_t available=end-*read,take=whole>=(double)available?available:(uint64_t)whole;
    *read+=take;s->fraction=fmax(0,s->fraction-(double)take);
}
void klj_audio_pcm_mix(float *output,size_t frames,unsigned channels,double rate){
    if(!output||!channels||!isfinite(rate)||rate<=0)return;
    atomic_fetch_add_explicit(&readers,1,memory_order_seq_cst);snapshot *items=atomic_load_explicit(&current,memory_order_seq_cst);
    if(items)for(size_t index=0;index<items->count;index++){
        source *s=atomic_load_explicit(&items->items[index],memory_order_seq_cst);if(!s||!atomic_load_explicit(&s->playing,memory_order_seq_cst))continue;
        double step=atomic_load(&s->rate)/rate;if(step<=0)continue;
        float left=atomic_load(&s->left),right=atomic_load(&s->right);int dual=atomic_load(&s->dualMono);
        uint64_t start=atomic_load_explicit(&s->read,memory_order_relaxed),r=start,w=atomic_load_explicit(&s->write,memory_order_acquire);bool underrun=false;
        // AOSP evaluates its curve at render-burst boundaries. Interpolate
        // between those gains in the existing mixer without Java callbacks or
        // per-sample curve locks. Only actually queued audio advances its clock.
        size_t playable=0;float first=1,last=1;
        if(s->gain){double available=fmax(0,(double)(w-start)-s->fraction);playable=available/step>=(double)frames?frames:(size_t)ceil(available/step);if(playable){first=s->gain(s->gainOwner,s->gainPosition);last=s->gain(s->gainOwner,s->gainPosition+(uint64_t)((playable-1)*step));}}
        for(size_t i=0;i<frames;i++){
            advance(s,&r,w);if(r>=w){underrun=true;break;}
            float gain=playable>1?first+(last-first)*(float)i/(float)(playable-1):first;
            if(s->channels>2&&channels>=s->channels&&dual==0){
                for(unsigned ch=0;ch<s->channels;ch++)output[i*channels+ch]+=finite_sample(sample(s,r,ch,s->fraction,w)*((ch&1)?right:left)*gain);
            }else{
                float l=0,rr=0;unsigned ln=0,rn=0;
                for(unsigned ch=0;ch<s->channels;ch++){
                    float value=sample(s,r,ch,s->fraction,w);
                    if(s->channels==1){l=rr=value;ln=rn=1;break;}
                    if(ch&1){rr+=value;rn++;}else{l+=value;ln++;}
                }
                l/=ln?ln:1;rr/=rn?rn:1;
                if(dual==1)l=rr=(l+rr)*0.5f;else if(dual==2)rr=l;else if(dual==3)l=rr;
                l=finite_sample(l*left*gain);rr=finite_sample(rr*right*gain);
                if(channels==1)output[i]+=0.5f*(l+rr);else{output[i*channels]+=l;output[i*channels+1]+=rr;}
            }
            s->fraction+=step;
        }
        advance(s,&r,w);
        s->gainPosition+=r-start;atomic_fetch_add(&s->position,r-start);atomic_store_explicit(&s->read,r,memory_order_release);
        if(s->clip){if(r>=atomic_load_explicit(&s->write,memory_order_acquire))atomic_store_explicit(&s->playing,false,memory_order_seq_cst);}
        else if(underrun)atomic_fetch_add(&s->underruns,1);
    }
    atomic_fetch_sub_explicit(&readers,1,memory_order_seq_cst);
}
