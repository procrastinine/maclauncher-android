#include "media-format.h"
#include <stdatomic.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
static _Atomic(const KLJMediaFormatApi *) formatApi;
static _Atomic(void *) formatLibrary;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t loadOnce=PTHREAD_ONCE_INIT;
static void load_library(void){
    Dl_info info={0};char file[PATH_MAX];
    if(!dladdr((const void*)klj_media_formats,&info)||!info.dli_fname)return;
    const char *slash=strrchr(info.dli_fname,'/');if(!slash)return;
    int size=snprintf(file,sizeof(file),"%.*s/libandroid-media.dylib",(int)(slash-info.dli_fname),info.dli_fname);
    if(size<0||(size_t)size>=sizeof(file))return;
    void *library=dlopen(file,RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"Android native media library could not load: %s\n",dlerror());return;}
    bool (*bind)(void*)=(bool(*)(void*))dlsym(library,"klj_media_format_bind");
    // Publish callbacks only after every dependency and static initializer has
    // completed. The complete library is retained by the shared process owner.
    if(!bind||!bind(library)){fputs("Android native media library could not bind\n",stderr);dlclose(library);}
}
static void ensure_library(void){if(!atomic_load_explicit(&formatApi,memory_order_acquire))pthread_once(&loadOnce,load_library);}
bool klj_media_format_install(const KLJMediaFormatApi *api,void *library){
    if(!api||!library)return false;
    pthread_mutex_lock(&lock);
    const KLJMediaFormatApi *old=atomic_load(&formatApi);
    bool okay=!old||old==api;
    if(!old){atomic_store(&formatLibrary,library);atomic_store_explicit(&formatApi,api,memory_order_release);}
    pthread_mutex_unlock(&lock);if(old&&okay)dlclose(library);return okay;
}
const KLJMediaFormatApi *klj_media_formats(void){
    ensure_library();
    const KLJMediaFormatApi *api=atomic_load_explicit(&formatApi,memory_order_acquire);
    if(!api){fputs("Android native media library was not initialized\n",stderr);abort();}
    return api;
}
void *klj_media_format_lookup(const char *name){ensure_library();void *library=atomic_load(&formatLibrary);return library&&name?dlsym(library,name):NULL;}
const char *klj_media_format_string(AMediaFormat *format,const char *key){const char *value=NULL;return klj_media_formats()->getString(format,key,&value)?value:NULL;}
