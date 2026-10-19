#include "audio-device.h"
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <Block.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const AudioObjectPropertyAddress outputAddress={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
static _Atomic uint32_t generation=1;
static int property(AudioObjectID id,AudioObjectPropertySelector selector,AudioObjectPropertyScope scope,void *value,UInt32 bytes){
    AudioObjectPropertyAddress address={selector,scope,kAudioObjectPropertyElementMain};UInt32 size=bytes;
    return AudioObjectGetPropertyData(id,&address,0,NULL,&size,value)==noErr&&size==bytes;
}
int klj_audio_device_list(uint32_t **devices,size_t *count){
    *devices=NULL;*count=0;
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 bytes=0;
    if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,NULL,&bytes)!=noErr||bytes%sizeof(AudioDeviceID))return -EIO;
    if(!bytes)return 0;
    AudioDeviceID *result=malloc(bytes);if(!result)return -ENOMEM;
    UInt32 capacity=bytes;
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&bytes,result)!=noErr||bytes>capacity||bytes%sizeof(AudioDeviceID)){free(result);return -EIO;}
    *devices=result;*count=bytes/sizeof(AudioDeviceID);return 0;
}
int klj_audio_device_default_output(uint32_t *device){
    *device=0;UInt32 bytes=sizeof(*device);
    return AudioObjectGetPropertyData(kAudioObjectSystemObject,&outputAddress,0,NULL,&bytes,device)==noErr&&bytes==sizeof(*device)?0:-EIO;
}
int klj_audio_device_get(uint32_t id,int input,KljAudioDevice *out){
    memset(out,0,sizeof(*out));out->id=id;if(!id)return 0;
    AudioObjectPropertyScope scope=input?kAudioObjectPropertyScopeInput:kAudioObjectPropertyScopeOutput;
    AudioObjectPropertyAddress address={kAudioDevicePropertyStreamConfiguration,scope,kAudioObjectPropertyElementMain};UInt32 bytes=0;
    if(AudioObjectGetPropertyDataSize(id,&address,0,NULL,&bytes)!=noErr||bytes<offsetof(AudioBufferList,mBuffers))return -EIO;
    AudioBufferList *buffers=malloc(bytes);if(!buffers)return -ENOMEM;
    UInt32 capacity=bytes;OSStatus result=AudioObjectGetPropertyData(id,&address,0,NULL,&bytes,buffers);
    if(result!=noErr||bytes>capacity||bytes<offsetof(AudioBufferList,mBuffers)||buffers->mNumberBuffers>(bytes - offsetof(AudioBufferList,mBuffers))/sizeof(AudioBuffer)){free(buffers);return -EIO;}
    for(UInt32 i=0;i<buffers->mNumberBuffers;i++){
        UInt32 channels=buffers->mBuffers[i].mNumberChannels;
        if(channels>UINT32_MAX-out->channels){free(buffers);return -EOVERFLOW;}
        out->channels+=channels;
    }
    free(buffers);
    property(id,kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,&out->alive,sizeof(out->alive));
    property(id,kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,&out->rate,sizeof(out->rate));
    if(!isfinite(out->rate)||out->rate<=0)out->rate=0;
    property(id,kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,&out->bufferFrames,sizeof(out->bufferFrames));
    property(id,kAudioDevicePropertyLatency,scope,&out->latencyFrames,sizeof(out->latencyFrames));
    property(id,kAudioDevicePropertySafetyOffset,scope,&out->safetyFrames,sizeof(out->safetyFrames));
    property(id,kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal,&out->transport,sizeof(out->transport));
    property(id,kAudioDevicePropertyJackIsConnected,scope,&out->jack,sizeof(out->jack));
    if(out->transport==kAudioDeviceTransportTypeBuiltIn)out->type=out->jack?4:2;
    else if(out->transport==kAudioDeviceTransportTypeBluetooth||out->transport==kAudioDeviceTransportTypeBluetoothLE)out->type=8;
    else if(out->transport==kAudioDeviceTransportTypeUSB)out->type=11;
    else if(out->transport==kAudioDeviceTransportTypeHDMI||out->transport==kAudioDeviceTransportTypeDisplayPort)out->type=9;
    else out->type=21; // Android TYPE_BUS for another physical transport.
    property(id,kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,&out->name,sizeof(out->name));
    property(id,kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,&out->uid,sizeof(out->uid));
    return 0;
}
void klj_audio_device_release(KljAudioDevice *description){
    if(description->name)CFRelease(description->name);if(description->uid)CFRelease(description->uid);
    description->name=description->uid=NULL;
}
struct KljAudioDeviceMonitor {
    dispatch_queue_t queue;AudioObjectPropertyListenerBlock listener;
    _Atomic int active;KljAudioDeviceChanged changed;void *owner;
};
static void destroyMonitor(KljAudioDeviceMonitor *monitor){Block_release(monitor->listener);dispatch_release(monitor->queue);free(monitor);}
KljAudioDeviceMonitor *klj_audio_device_monitor(KljAudioDeviceChanged changed,void *owner){
    if(!changed)return NULL;
    KljAudioDeviceMonitor *monitor=calloc(1,sizeof(*monitor));if(!monitor)return NULL;
    monitor->changed=changed;monitor->owner=owner;
    monitor->queue=dispatch_queue_create("maclauncher.audio-device",DISPATCH_QUEUE_SERIAL);
    if(!monitor->queue){free(monitor);return NULL;}
    dispatch_queue_set_specific(monitor->queue,monitor,monitor,NULL);
    monitor->listener=Block_copy(^(UInt32 count,const AudioObjectPropertyAddress *addresses){
        (void)count;(void)addresses;
        if(!atomic_load_explicit(&monitor->active,memory_order_acquire))return;
        uint32_t previous=atomic_load_explicit(&generation,memory_order_relaxed),next;
        do{next=previous==INT_MAX?1:previous+1;}while(!atomic_compare_exchange_weak_explicit(&generation,&previous,next,memory_order_relaxed,memory_order_relaxed));
        monitor->changed(monitor->owner);
    });
    atomic_store_explicit(&monitor->active,1,memory_order_release);
    if(AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject,&outputAddress,monitor->queue,monitor->listener)!=noErr){destroyMonitor(monitor);return NULL;}
    return monitor;
}
int klj_audio_device_monitor_close(KljAudioDeviceMonitor *monitor){
    if(!monitor)return 0;
    atomic_store_explicit(&monitor->active,0,memory_order_release);
    if(AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject,&outputAddress,monitor->queue,monitor->listener)!=noErr)return -EIO;
    if(dispatch_get_specific(monitor))dispatch_async(monitor->queue,^{destroyMonitor(monitor);});
    else {dispatch_sync(monitor->queue,^{});destroyMonitor(monitor);}
    return 0;
}
uint32_t klj_audio_device_generation(void){return atomic_load_explicit(&generation,memory_order_relaxed);}
