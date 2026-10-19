#include "native-clock/clock-domain.h"
static int klj_reader_present(void*pointer,CVPixelBufferRef pixels,int64_t media,int64_t requested,void(*completed)(void*,int64_t),void*context){
    (void)requested;int result=klm_reader_publish(pointer,pixels,media);
    if(!result&&completed){struct timespec now;klj_clock_read(1,&now);completed(context,(int64_t)now.tv_sec*1000000000+now.tv_nsec);}return result;
}
int klj_media_reader_surface(void*window,KLJMediaSurface*out){
    if(!window||!out)return AMEDIA_ERROR_INVALID_PARAMETER;
    pthread_mutex_lock(&klj_readers_lock);klm_reader*reader=NULL;
    for(klj_reader_link*link=klj_readers;link;link=link->next)if(link->reader->window==window){reader=link->reader;reader->refs++;break;}
    pthread_mutex_unlock(&klj_readers_lock);if(!reader)return AMEDIA_ERROR_UNSUPPORTED;
    *out=(KLJMediaSurface){reader,klj_reader_retain,klj_reader_release,klj_reader_present};return AMEDIA_OK;
}
