#include "media-bridge.h"
#include "host-packets.h"
#include "media-platform.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int transfer(struct KljArtMedia *request,void *bytes,size_t length){
    request->length=length;
    if(length&&!bytes)return -12;
    if(length>request->capacity){request->result=1;free(bytes);return 0;}
    if(length)memcpy((void*)(uintptr_t)request->data,bytes,length);
    free(bytes);request->result=0;return 0;
}

int64_t maclauncher_art_media(uint64_t address,uint64_t size){
    if(!address||size!=sizeof(struct KljArtMedia))return -22;
 int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(struct KljArtMedia),KLJ_ART_PACKET_READ_WRITE,&address);if(packet_error)return packet_error;
    struct KljArtMedia *r=(void*)(uintptr_t)address;
    switch(r->operation){
        case KLJ_ART_MEDIA_DEVICES:{char *text=klj_media_devices();if(!text)return -12;return transfer(r,text,strlen(text));}
        case KLJ_ART_MEDIA_CONFIGURE:
            r->result=klj_media_configure((const char*)(uintptr_t)r->data,(const char*)(uintptr_t)r->other);return 0;
        case KLJ_ART_MEDIA_AUTHORIZED:r->result=klj_media_authorized(r->first);return 0;
        case KLJ_ART_MEDIA_SHUTDOWN:klj_media_shutdown();break;
        case KLJ_ART_MEDIA_AUDIO_OPEN:r->result=klj_media_audio_open(r->first,r->second);return 0;
        case KLJ_ART_MEDIA_AUDIO_START:r->result=klj_media_audio_start(r->token);return 0;
        case KLJ_ART_MEDIA_AUDIO_READ:r->result=klj_media_audio_read(r->token,(int16_t*)(uintptr_t)r->data,(size_t)r->capacity);return 0;
        case KLJ_ART_MEDIA_AUDIO_STOP:klj_media_audio_stop(r->token);break;
        case KLJ_ART_MEDIA_AUDIO_CLOSE:klj_media_audio_close(r->token);break;
        case KLJ_ART_MEDIA_CAMERA_SIZES:{size_t count=0;int *values=klj_media_camera_sizes(&count);if(count>SIZE_MAX/sizeof(int)){free(values);return -12;}return transfer(r,values,count*sizeof(int));}
        case KLJ_ART_MEDIA_CAMERA_OPEN:r->result=klj_media_camera_open();return 0;
        case KLJ_ART_MEDIA_CAMERA_START:r->result=klj_media_camera_start(r->token,r->first,r->second);return 0;
        case KLJ_ART_MEDIA_CAMERA_READ:r->result=klj_media_camera_read(r->token,(void*)(uintptr_t)r->data,(size_t)r->capacity);return 0;
        case KLJ_ART_MEDIA_CAMERA_STOP:klj_media_camera_stop(r->token);break;
        case KLJ_ART_MEDIA_CAMERA_CLOSE:klj_media_camera_close(r->token);break;
        default:return -22;
    }
    r->result=0;return 0;
}
