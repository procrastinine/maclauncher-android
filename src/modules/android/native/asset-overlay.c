// The original Android NDK supplier owns every AAsset and AConfiguration API.
// This module retains the prepared-file/OBB filesystem boundary only.
#ifndef KLEPTON_NDK_SOURCE
#error Define KLEPTON_NDK_SOURCE to the adapted pinned kl_ndk.c
#endif
#define kl_ndk_lookup kl_upstream_ndk_lookup
#include KLEPTON_NDK_SOURCE
#undef kl_ndk_lookup
#include <stdatomic.h>
#include <sys/stat.h>
#include "native-invocation.h"

extern void *klj_resolve_import(const char *);
// Klepton's native/XR Activity constructor keeps its existing entry point.
// Its assets borrow the current original Application's resource manager too.
void *kl_ndk_asset_manager(void){
    JNIEnv *env=klj_guest_thread_environment();
    if(!env||(*env)->ExceptionCheck(env)||(*env)->PushLocalFrame(env,8)<0)return NULL;
    jclass activity_thread=(*env)->FindClass(env,"android/app/ActivityThread");
    jmethodID current=activity_thread?(*env)->GetStaticMethodID(env,activity_thread,"currentApplication","()Landroid/app/Application;"):NULL;
    jobject application=current?(*env)->CallStaticObjectMethodA(env,activity_thread,current,NULL):NULL;
    jclass context=application?(*env)->FindClass(env,"android/content/Context"):NULL;
    jmethodID get_assets=context?(*env)->GetMethodID(env,context,"getAssets","()Landroid/content/res/AssetManager;"):NULL;
    jobject assets=get_assets?(*env)->CallObjectMethodA(env,application,get_assets,NULL):NULL;
    void *(*from_java)(JNIEnv *,jobject)=klj_resolve_import("AAssetManager_fromJava");
    void *manager=assets&&from_java?from_java(env,assets):NULL;
    (*env)->PopLocalFrame(env,NULL);return manager;
}

extern int klj_obb_asset_window(const char *,int64_t *,int64_t *);
int klj_asset_open_fd(const char *name,int64_t *offset,int64_t *length){
    if(!name||!*name||*name=='/'){errno=EINVAL;return -1;}
    for(const char *part=name;*part;){
        const char *end=strchr(part,'/');size_t size=end?(size_t)(end-part):strlen(part);
        if(size==2&&part[0]=='.'&&part[1]=='.'){errno=EINVAL;return -1;}
        if(!end)break;part=end+1;
    }
    char file[4096];
    int size=snprintf(file,sizeof file,"%s/%s",g_asset_root,name);
    if(size<0||(size_t)size>=sizeof file){errno=ENAMETOOLONG;return -1;}
    int fd=open(file,O_RDONLY|O_CLOEXEC);
    if(fd<0)return klj_obb_asset_window(file,offset,length);
    struct stat info;
    if(fstat(fd,&info)){int saved=errno;close(fd);errno=saved;return -1;}
    if(!S_ISREG(info.st_mode)){int error=S_ISDIR(info.st_mode)?EISDIR:EINVAL;close(fd);errno=error;return -1;}
    *offset=0;*length=info.st_size;return fd;
}

static pthread_once_t asset_trace_once=PTHREAD_ONCE_INIT;
static int asset_trace_enabled;
static _Atomic unsigned asset_trace_count;
static void initialize_asset_trace(void){const char *value=getenv("KL_TRACE_ASSETS");asset_trace_enabled=value&&*value&&strcmp(value,"0");}
void klj_asset_opened(const char *name,int mode,int64_t bytes,int error){
    pthread_once(&asset_trace_once,initialize_asset_trace);
    if(asset_trace_enabled){
        unsigned index=atomic_fetch_add_explicit(&asset_trace_count,1,memory_order_relaxed);
        if(index<256)fprintf(stderr,"[android-assets] open name=\"%s\" mode=%d bytes=%lld error=%d\n",name?name:"",mode,(long long)bytes,error);
        else if(index==256)fprintf(stderr,"[android-assets] further asset-open diagnostics suppressed\n");
    }
}
extern void *klj_ndk_choreographer_lookup(const char *);
void *kl_ndk_lookup(const char *name){void *display=klj_ndk_choreographer_lookup(name);return display?display:kl_upstream_ndk_lookup(name);}
