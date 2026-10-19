#include "media-surface.h"
#include <stdatomic.h>
// The graphics library supplies its existing native-window consumer. Keeping
// this registration in the shared process library avoids a media/graphics DSO
// load cycle; the retained Surface itself owns subsequent frame handoffs.
static _Atomic(int(*)(JNIEnv*,jobject,KLJMediaSurface*)) javaOwner;
static _Atomic(int(*)(void*,KLJMediaSurface*)) windowOwner;
void klj_media_surface_install(int(*javaSurface)(JNIEnv*,jobject,KLJMediaSurface*),int(*nativeWindow)(void*,KLJMediaSurface*)){
    atomic_store_explicit(&javaOwner,javaSurface,memory_order_release);
    atomic_store_explicit(&windowOwner,nativeWindow,memory_order_release);
}
int klj_codec_surface_java(JNIEnv*env,jobject surface,KLJMediaSurface*out){
    int(*acquire)(JNIEnv*,jobject,KLJMediaSurface*)=atomic_load_explicit(&javaOwner,memory_order_acquire);
    return acquire?acquire(env,surface,out):-10002;
}
int klj_media_surface_acquire(void*window,KLJMediaSurface*out){
    int(*acquire)(void*,KLJMediaSurface*)=atomic_load_explicit(&windowOwner,memory_order_acquire);
    if(acquire){int result=acquire(window,out);if(result!=-10002)return result;}
    return klj_media_reader_surface(window,out);
}
