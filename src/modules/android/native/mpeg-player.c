// Compile the pinned Klepton decoder once, with bounded allocations and actual
// AudioQueue error capture. JNI and synthetic Klepton peers share this code.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <AudioToolbox/AudioToolbox.h>
#include "kl_mediaplayer.h"

typedef union {
    max_align_t align;
    struct { size_t size; _Atomic int error; } value;
} Allocation;
static _Atomic size_t allocated;
#define MPEG_MEMORY_LIMIT (256u * 1024u * 1024u)
static void *mpeg_malloc(size_t size) {
    if(size> MPEG_MEMORY_LIMIT || size>SIZE_MAX-sizeof(Allocation))return NULL;
    size_t old=atomic_load(&allocated);
    do { if(size>MPEG_MEMORY_LIMIT-old)return NULL; }
    while(!atomic_compare_exchange_weak(&allocated,&old,old+size));
    Allocation *a=malloc(sizeof(*a)+size);
    if(!a){atomic_fetch_sub(&allocated,size);return NULL;}
    a->value.size=size;atomic_init(&a->value.error,0);return a+1;
}
static void mpeg_free(void *p) {
    if(!p)return;Allocation *a=(Allocation *)p-1;
    atomic_fetch_sub(&allocated,a->value.size);free(a);
}
static void *mpeg_calloc(size_t count,size_t size) {
    if(size&&count>SIZE_MAX/size)return NULL;
    void *p=mpeg_malloc(count*size);if(p)memset(p,0,count*size);return p;
}
static void *mpeg_realloc(void *p,size_t size) {
    if(!p)return mpeg_malloc(size);
    void *next=mpeg_malloc(size);if(!next)return NULL;
    size_t old=((Allocation *)p-1)->value.size;
    memcpy(next,p,old<size?old:size);mpeg_free(p);return next;
}
static OSStatus record_error(kl_mediaplayer *player,OSStatus status) {
    if(player&&status){int expected=0;atomic_compare_exchange_strong(&((Allocation *)player-1)->value.error,&expected,(int)status);}
    return status;
}

#define malloc mpeg_malloc
#define calloc mpeg_calloc
#define realloc mpeg_realloc
#define free mpeg_free
#define AudioQueueNewOutput(...) record_error(mp,AudioQueueNewOutput(__VA_ARGS__))
#define AudioQueueAllocateBuffer(...) record_error(mp,AudioQueueAllocateBuffer(__VA_ARGS__))
#define AudioQueueEnqueueBuffer(...) record_error(mp,AudioQueueEnqueueBuffer(__VA_ARGS__))
#define AudioQueueStart(...) record_error(mp,AudioQueueStart(__VA_ARGS__))
#define AudioQueuePause(...) record_error(mp,AudioQueuePause(__VA_ARGS__))
#define AudioQueueStop(...) record_error(mp,AudioQueueStop(__VA_ARGS__))
#define AudioQueueSetParameter(...) record_error(mp,AudioQueueSetParameter(__VA_ARGS__))
#include KLEPTON_MPEG_SOURCE
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef AudioQueueNewOutput
#undef AudioQueueAllocateBuffer
#undef AudioQueueEnqueueBuffer
#undef AudioQueueStart
#undef AudioQueuePause
#undef AudioQueueStop
#undef AudioQueueSetParameter

int klj_mpeg_error(kl_mediaplayer *player) {
    return player?atomic_load(&((Allocation *)player-1)->value.error):EINVAL;
}
int klj_mpeg_ready(kl_mediaplayer *player) {
    if(!player||!player->queue||klj_mpeg_error(player))return 0;
    for(int i=0;i<KL_MP_NBUF;i++)if(!player->bufs[i])return 0;
    return 1;
}
int klj_mpeg_prepare_bytes(kl_mediaplayer *player,const void *bytes,size_t size) {
    if(!player||player->have_source||!bytes||!size||size>64u*1024u*1024u)return 0;
    player->have_source=1;
    snprintf(player->path,sizeof player->path,"memory audio");
    klj_mpeg_prepare_data(player,bytes,(long)size);
    return klj_mpeg_ready(player);
}
int klj_mpeg_start(kl_mediaplayer *player) {
    if(!klj_mpeg_ready(player))return 0;
    pthread_mutex_lock(&player->mu);int complete=player->complete;pthread_mutex_unlock(&player->mu);
    if(complete)kl_mediaplayer_stop(player);
    kl_mediaplayer_start(player);
    return !klj_mpeg_error(player)&&kl_mediaplayer_is_playing(player);
}
int klj_mpeg_volume(kl_mediaplayer *mp,float left,float right) {
    if(!klj_mpeg_ready(mp))return 0;
    kl_mediaplayer_set_volume(mp,fmaxf(left,right));
    float pan=left==right?0:left>right?-(1-right/left):(1-left/right);
    record_error(mp,AudioQueueSetParameter(mp->queue,kAudioQueueParam_Pan,pan));
    return !klj_mpeg_error(mp);
}
size_t klj_mpeg_allocated_bytes(void){return atomic_load(&allocated);}
