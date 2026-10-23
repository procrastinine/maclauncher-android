// Original AIM audio-ring transport into the same physical PCM/CoreAudio owner.
// Android port/patch/stream policy remains in the original guest HAL and server.
#include "audio-pcm.h"
#include "host-packets.h"
#include "audio-device.h"
#include "media-platform.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

typedef struct {uint32_t present,rate,channels,buffer,latency,reserved;char name[64];} Device;
typedef struct {Device output,input;} Devices;
typedef struct {uint32_t direction,format,rate,channels;int32_t fd;uint32_t reserved;uint64_t length,stream;} Open;
typedef struct {
 uint32_t capacity,frameBytes;_Atomic uint64_t write,read,xruns,callbacks,callbackNs;
 _Atomic uint32_t stampSeq,latency;_Atomic uint64_t stampFrames;_Atomic int64_t stampNs;
 _Atomic uint32_t peakBits,detached;_Atomic uint64_t mark;_Atomic int64_t markNs;
 _Atomic uint64_t latencyCount,latencySum,latencyMax;
} Ring;
_Static_assert(sizeof(Open)==40,"Original audio Open ABI");
_Static_assert(offsetof(Ring,stampFrames)==56,"Original audio Ring ABI");
typedef struct Stream {
 struct Stream *next;uint64_t id;Ring *ring;size_t length;unsigned capacity,channels,rate,format,direction;uint64_t capture;
 void *pcm;float *scratch;pthread_t thread;pthread_mutex_t lock;pthread_cond_t changed;
 int playing,quit;uint64_t base,previous;
} Stream;
static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static Stream *streams;static uint64_t nextId=1;
static uint64_t now(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;}
static pthread_once_t mediaOnce=PTHREAD_ONCE_INIT;
static void configureMedia(void){
 const char *selected=getenv("MACLAUNCHER_ANDROID_AUDIO_INPUT_UID");if(!selected)return;
 int result=klj_media_configure(selected,"");
 if(result&&result!=KLJ_MEDIA_CONFIGURED)fprintf(stderr,"Selected audio input could not be found (%d)\n",result);
}
static int describeDevice(uint32_t id,int input,Device *out){
 if(!id)return 0;KljAudioDevice device;int error=klj_audio_device_get(id,input,&device);if(error)return error;
 if(!device.rate||device.rate>UINT32_MAX||!device.channels){klj_audio_device_release(&device);return -5;}
 out->rate=(uint32_t)llround(device.rate);out->buffer=device.bufferFrames;out->channels=device.channels;
 uint64_t latency=(uint64_t)device.latencyFrames+device.safetyFrames;out->latency=latency>UINT32_MAX?UINT32_MAX:(uint32_t)latency;
 if(device.name)CFStringGetCString(device.name,out->name,sizeof(out->name),kCFStringEncodingUTF8);
 klj_audio_device_release(&device);out->present=1;return 0;
}
static int devices(Devices *out){
 pthread_once(&mediaOnce,configureMedia);memset(out,0,sizeof(*out));uint32_t id=0;
 int error=klj_audio_device_default_output(&id);if(error)return error;
 error=describeDevice(id,0,&out->output);if(error)return error;
 uint32_t input=klj_media_audio_device();if(input)describeDevice(input,1,&out->input);
 return 0;
}
static void timestamp(Stream *s,uint64_t position,uint64_t ns){
 Ring *r=s->ring;uint32_t seq=atomic_load_explicit(&r->stampSeq,memory_order_relaxed);
 atomic_store_explicit(&r->stampSeq,seq+1,memory_order_relaxed);atomic_thread_fence(memory_order_release);
 atomic_store_explicit(&r->stampFrames,position,memory_order_relaxed);atomic_store_explicit(&r->stampNs,(int64_t)ns,memory_order_relaxed);atomic_store_explicit(&r->stampSeq,seq+2,memory_order_release);
 atomic_fetch_add_explicit(&r->callbacks,1,memory_order_relaxed);
}
static void stamp(Stream *s){
 uint64_t consumed=klj_audio_pcm_position(s->pcm);if(consumed==s->previous)return;s->previous=consumed;
 timestamp(s,s->base+consumed,now());
 atomic_store_explicit(&s->ring->read,s->base+consumed,memory_order_release);
 atomic_store_explicit(&s->ring->xruns,klj_audio_pcm_underruns(s->pcm),memory_order_relaxed);
}
static void capture(Stream *s){
 int got=klj_media_audio_read_frames(s->capture,s->scratch,256);if(got<=0)return;
 Ring *r=s->ring;uint64_t write=atomic_load_explicit(&r->write,memory_order_relaxed),read=atomic_load_explicit(&r->read,memory_order_acquire),used=write-read;
 if(used>s->capacity){atomic_store(&r->detached,1);s->playing=0;klj_media_audio_stop(s->capture);return;}
 size_t frames=(size_t)got,free=s->capacity-(size_t)used;if(frames>free)frames=free;
 size_t first=frames<s->capacity-write%s->capacity?frames:s->capacity-write%s->capacity;
 unsigned char *data=(void*)((char*)r+256);size_t bytes=r->frameBytes;
 if(first)memcpy(data+(write%s->capacity)*bytes,s->scratch,first*bytes);
 if(first<frames)memcpy(data,(unsigned char*)s->scratch+first*bytes,(frames-first)*bytes);
 if(frames<(size_t)got)atomic_fetch_add_explicit(&r->xruns,(size_t)got-frames,memory_order_relaxed);
 float peak=0;for(size_t i=0;i<(size_t)got*s->channels;i++){float value;if(s->format==2)value=s->scratch[i];else{int16_t n;memcpy(&n,(unsigned char*)s->scratch+i*2,2);value=n/32768.0f;}if(isfinite(value)&&fabsf(value)>peak)peak=fabsf(value);}
 uint32_t bits;memcpy(&bits,&peak,4);if(bits>atomic_load(&r->peakBits))atomic_store(&r->peakBits,bits);
 uint64_t latency=(uint64_t)atomic_load(&r->latency)*1000000000/s->rate,time=now();timestamp(s,write,time>latency?time-latency:0);
 atomic_store_explicit(&r->write,write+frames,memory_order_release);
}
static void *feed(void *value){
 Stream *s=value;pthread_setname_np("MacLauncher ART PCM");
 pthread_mutex_lock(&s->lock);
 while(!s->quit){
  while(!s->playing&&!s->quit)pthread_cond_wait(&s->changed,&s->lock);if(s->quit)break;
  if(s->direction){capture(s);pthread_mutex_unlock(&s->lock);usleep(1000);pthread_mutex_lock(&s->lock);continue;}
  stamp(s);
  uint64_t read=s->base+klj_audio_pcm_position(s->pcm)+klj_audio_pcm_queued(s->pcm),write=atomic_load_explicit(&s->ring->write,memory_order_acquire),count=write-read;
  if(count>s->capacity){atomic_store(&s->ring->detached,1);s->playing=0;klj_audio_pcm_state(s->pcm,1);continue;}
  size_t frames=(size_t)count;if(frames>256)frames=256;
  if(frames){const unsigned char *data=(void*)((char*)s->ring+256);float peak=0;
   for(size_t f=0;f<frames;f++)for(unsigned c=0;c<s->channels;c++){
    size_t at=((read+f)%s->capacity)*s->channels+c;float sample;
    if(s->format==2)memcpy(&sample,data+at*4,4);else{int16_t n;memcpy(&n,data+at*2,2);sample=n/32768.0f;}
    s->scratch[f*s->channels+c]=sample;if(isfinite(sample)&&fabsf(sample)>peak)peak=fabsf(sample);
   }
   // This PCM source is paused while control owns the lock, so a bounded
   // write cannot deadlock stop/close behind a full playing queue.
   uint64_t queued=klj_audio_pcm_queued(s->pcm);if(queued<s->capacity){if(frames>s->capacity-queued)frames=s->capacity-queued;
    klj_audio_pcm_write(s->pcm,s->scratch,(int)frames);
    uint32_t bits;memcpy(&bits,&peak,4);if(bits>atomic_load(&s->ring->peakBits))atomic_store(&s->ring->peakBits,bits);
   }
  }
  stamp(s);pthread_mutex_unlock(&s->lock);usleep(1000);pthread_mutex_lock(&s->lock);
 }
 pthread_mutex_unlock(&s->lock);return NULL;
}
static int openStream(Open *o){
 pthread_once(&mediaOnce,configureMedia);
 if(o->direction>1)return -22;if(o->direction&&!klj_media_audio_device())return -13;
 unsigned bytes=o->format==1?2:o->format==2?4:0;if(!bytes||!o->channels||o->channels>8||o->rate<8000||o->rate>192000||o->length<256||o->length>SIZE_MAX)return -22;
 struct stat st;if(fstat(o->fd,&st)||st.st_size<0||(uint64_t)st.st_size<o->length)return -22;
 Ring *ring=mmap(NULL,(size_t)o->length,PROT_READ|PROT_WRITE,MAP_SHARED,o->fd,0);if(ring==MAP_FAILED)return -12;
 if(!ring->capacity||ring->frameBytes!=o->channels*bytes||(uint64_t)ring->capacity*ring->frameBytes>o->length-256){munmap(ring,(size_t)o->length);return -22;}
 Stream *s=calloc(1,sizeof(*s));if(!s){munmap(ring,(size_t)o->length);return -12;}
 s->ring=ring;s->length=o->length;s->capacity=ring->capacity;s->channels=o->channels;s->rate=o->rate;s->format=o->format;s->direction=o->direction;s->base=atomic_load(&ring->read);
 s->scratch=calloc(256*s->channels,sizeof(float));
 if(s->direction){int64_t token=klj_media_audio_open_format(s->rate,s->channels,s->format);if(token>0)s->capture=(uint64_t)token;}else s->pcm=klj_audio_pcm_create(s->capacity,s->channels,s->rate);
 if(s->capture){KljAudioDevice input;if(!klj_audio_device_get(klj_media_audio_device(),1,&input)){double frames=input.rate>0?((double)input.latencyFrames+input.safetyFrames)*s->rate/input.rate:0;atomic_store(&ring->latency,frames>UINT32_MAX?UINT32_MAX:(uint32_t)ceil(frames));klj_audio_device_release(&input);}}
 if(!s->scratch||(!s->pcm&&!s->capture)){if(s->capture)klj_media_audio_close(s->capture);if(s->pcm)klj_audio_pcm_destroy(s->pcm);free(s->scratch);munmap(ring,s->length);free(s);return -19;}
 pthread_mutex_init(&s->lock,NULL);pthread_cond_init(&s->changed,NULL);int error=pthread_create(&s->thread,NULL,feed,s);
 if(error){pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->lock);if(s->capture)klj_media_audio_close(s->capture);if(s->pcm)klj_audio_pcm_destroy(s->pcm);free(s->scratch);munmap(ring,s->length);free(s);return -11;}
 pthread_mutex_lock(&registry);s->id=nextId++;s->next=streams;streams=s;pthread_mutex_unlock(&registry);o->stream=s->id;fprintf(stderr,"Shared CoreAudio PCM opened: %u Hz %u channels\n",s->rate,s->channels);return 0;
}
int64_t maclauncher_art_audio(uint32_t function,uint64_t address,uint64_t size){
 if(!address)return -22;
 if(function==1){if(size!=sizeof(Devices))return -22;int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(Devices),KLJ_ART_PACKET_READ_WRITE,&address);if(packet_error)return packet_error;return devices((void*)(uintptr_t)address);}
 if(function==2){if(size!=sizeof(Open))return -22;int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(Open),KLJ_ART_PACKET_READ_WRITE,&address);if(packet_error)return packet_error;return openStream((void*)(uintptr_t)address);}
 if(size!=8||function<3||function>5)return -22;
 int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(uint64_t),KLJ_ART_PACKET_READ,&address);if(packet_error)return packet_error;
 uint64_t id=*(uint64_t*)(uintptr_t)address;pthread_mutex_lock(&registry);Stream **slot=&streams;while(*slot&&(*slot)->id!=id)slot=&(*slot)->next;Stream *s=*slot;if(!s){pthread_mutex_unlock(&registry);return -22;}
 pthread_mutex_lock(&s->lock);
 int result=0;
 if(function==3&&s->direction){if(klj_media_audio_start(s->capture)){s->playing=1;pthread_cond_signal(&s->changed);}else result=-19;}
 else if(function==3){
  uint64_t produced=atomic_load_explicit(&s->ring->write,memory_order_acquire),fed=s->base+klj_audio_pcm_position(s->pcm)+klj_audio_pcm_queued(s->pcm);
  // The original HAL may discard its stopped ring. Retire exactly the same
  // pending frames from the physical source before restarting it.
  if(produced<fed){klj_audio_pcm_state(s->pcm,2);s->base=atomic_load(&s->ring->read);s->previous=0;}
  s->playing=1;klj_audio_pcm_state(s->pcm,0);pthread_cond_signal(&s->changed);
 }
 else {s->playing=0;if(s->direction)klj_media_audio_stop(s->capture);else{klj_audio_pcm_state(s->pcm,1);stamp(s);}}
 if(function==5){*slot=s->next;s->quit=1;pthread_cond_signal(&s->changed);}
 pthread_mutex_unlock(&s->lock);pthread_mutex_unlock(&registry);
 if(function==5){pthread_join(s->thread,NULL);uint64_t consumed=s->direction?atomic_load(&s->ring->write):klj_audio_pcm_position(s->pcm),queued=s->direction?atomic_load(&s->ring->write)-atomic_load(&s->ring->read):klj_audio_pcm_queued(s->pcm);fprintf(stderr,"Shared CoreAudio PCM retired: consumed=%llu queued=%llu\n",(unsigned long long)consumed,(unsigned long long)queued);if(s->capture)klj_media_audio_close(s->capture);if(s->pcm)klj_audio_pcm_destroy(s->pcm);pthread_cond_destroy(&s->changed);pthread_mutex_destroy(&s->lock);munmap(s->ring,s->length);free(s->scratch);free(s);}
 return result;
}
