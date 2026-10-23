#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "kl_nativeactivity.h"
#include "host-callbacks.h"
#include "native-import-registry.h"
#include "native-invocation.h"
#include "native-activity-callbacks.h"

/* Reuse the pinned NDK ABI and loader, with real HotSpot references. This is
 * deliberately not kl_na_create, whose driver owns a synthetic JNI universe. */
extern JNIEnv *klj_guest_env(JNIEnv*);
extern JavaVM *klj_guest_vm(JavaVM*);
extern void *klj_guest_runtime_symbol(const char *);
extern void *klj_guest_image_symbol(jlong,const char *);
typedef struct {
    kl_ANativeActivity activity;
    kl_ANativeActivityCallbacks callbacks;
    pthread_t owner;
    jlong image;
    int provider_owned;
    JavaVM *vm;
    jobject object;
    jclass entry;
    KLHostCallbacks *bindings;
    jmethodID pump;
    char *paths[3];
    void *window;
} Activity;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static Activity *current;
static void (*prepare_looper)(void);
static int (*pump_looper)(int);
static void *(*asset_manager)(JNIEnv *,jobject);
static int installed;
static void fail(JNIEnv *env,const char *text){if((*env)->ExceptionCheck(env))return;jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");if(type)(*env)->ThrowNew(env,type,text);}
static void request_owner(kl_ANativeActivity *native,int kind,int first,int second){
    /* Take only a local JNI reference under the lock. Never call guest Java or
     * wait for its owner while holding the native lifetime lock. */
    pthread_mutex_lock(&gate);Activity *value=current;JavaVM *vm=value&&native==&value->activity?value->vm:NULL;pthread_mutex_unlock(&gate);
    if(!vm)return;
    JNIEnv *env=NULL;int attached=0;jint status=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(status==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,(void**)&env,NULL)!=JNI_OK)return;attached=1;}else if(status!=JNI_OK)return;
    pthread_mutex_lock(&gate);value=current;int live=value&&native==&value->activity;jobject object=live?(*env)->NewLocalRef(env,value->object):NULL;KLHostCallbacks *bindings=live?kl_host_callbacks_retain(value->bindings):NULL;pthread_mutex_unlock(&gate);
    if(object&&bindings){kl_host_native_activity_request(env,bindings,kl_host_bind_KLHost_android_app_NativeActivity(env,bindings,object),kind,first,second);(*env)->ExceptionCheck(env);}
    if(object)(*env)->DeleteLocalRef(env,object);kl_host_callbacks_release(env,bindings);
    if((*env)->ExceptionCheck(env)&&attached){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}
    if(attached)(*vm)->DetachCurrentThread(vm);
}
static void request_finish(kl_ANativeActivity *a){request_owner(a,0,0,0);}
static void request_format(kl_ANativeActivity *a,int format){request_owner(a,1,format,0);}
static void request_flags(kl_ANativeActivity *a,unsigned add,unsigned remove){request_owner(a,2,(int)add,(int)remove);}
static void request_show(kl_ANativeActivity *a,unsigned flags){request_owner(a,3,(int)flags,0);}
static void request_hide(kl_ANativeActivity *a,unsigned flags){request_owner(a,4,(int)flags,0);}
static void *lookup(const char *name){
    if(!strcmp(name,"ANativeActivity_finish"))return (void*)request_finish;
    if(!strcmp(name,"ANativeActivity_setWindowFormat"))return (void*)request_format;
    if(!strcmp(name,"ANativeActivity_setWindowFlags"))return (void*)request_flags;
    if(!strcmp(name,"ANativeActivity_showSoftInput"))return (void*)request_show;
    if(!strcmp(name,"ANativeActivity_hideSoftInput"))return (void*)request_hide;
    return NULL;
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeBinding_prepareNativeActivity(JNIEnv *env,jclass type){
    if(installed)return;
    prepare_looper=klj_guest_runtime_symbol("kl_ndk_prepare_looper");pump_looper=klj_guest_runtime_symbol("kl_ndk_pump_looper");
    void *(*resolve)(const char *)=klj_guest_runtime_symbol("klj_resolve_import");asset_manager=resolve?resolve("AAssetManager_fromJava"):NULL;
    int (*install)(KljImportResolver)=klj_guest_runtime_symbol("klj_register_import_resolver");
    if(!prepare_looper||!pump_looper||!asset_manager||!install){fail(env,"NativeActivity requires the existing Klepton NDK services");return;}
    if(!install(lookup)){fail(env,"Native import registration allocation failed");return;}installed=1;
}
static char *copy_path(JNIEnv *env,jbyteArray bytes){
    if(!bytes)return NULL;jsize count=(*env)->GetArrayLength(env,bytes);if(count<1){fail(env,"NativeActivity data path is empty");return NULL;}
    char *out=malloc((size_t)count+1);if(!out){fail(env,"NativeActivity path allocation failed");return NULL;}
    (*env)->GetByteArrayRegion(env,bytes,0,count,(jbyte*)out);out[count]=0;
    if((*env)->ExceptionCheck(env)||out[0]!='/'||memchr(out,0,(size_t)count)){free(out);fail(env,"Invalid NativeActivity data path");return NULL;}return out;
}
/* The image provider owns only callback execution/JNI carriers. Existing NDK
 * resources remain opaque objects of their original host supplier. Their guest
 * consumers require that supplier's veneers; stock Bionic C++ APIs cannot
 * consume these pointers. No resource implementation is substituted here. */
#define ACTIVITY_CALL(value,function,second,third,statement) do { \
    if(!klj_native_activity_callback(env,(value)->image,(const void*)(function), \
            (uintptr_t)&(value)->activity,(uint64_t)(uintptr_t)(second), \
            (uint64_t)(uintptr_t)(third),NULL))KLJ_INVOKE_GUEST(env,statement); \
}while(0)
static void invoke_create(JNIEnv *env,Activity *value,void (*create)(kl_ANativeActivity*,void*,size_t),void *data,size_t size){
    ACTIVITY_CALL(value,create,data,size,create(&value->activity,data,size));
}
static void release_saved(JNIEnv *env,Activity *value,void *data){
    if(!data)return;
    if(!value->provider_owned){free(data);return;}
    /* Original libc allocation belongs to the application's real dependency
     * graph. Never send it to Darwin free or infer ownership from its address. */
    jthrowable first=(*env)->ExceptionOccurred(env);if(first)(*env)->ExceptionClear(env);
    void *release=klj_guest_image_symbol(value->image,"free");
    if(!release)fail(env,"NativeActivity saved state allocator is unavailable");
    else if(!klj_native_activity_callback(env,value->image,release,(uintptr_t)data,0,0,NULL))
        fail(env,"NativeActivity saved state allocator has no image owner");
    if(first){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->Throw(env,first);(*env)->DeleteLocalRef(env,first);}
}
static void dispose(JNIEnv *env,Activity *value){
    pthread_mutex_lock(&gate);if(current==value)current=NULL;pthread_mutex_unlock(&gate);
    if(value->object)(*env)->DeleteGlobalRef(env,value->object);if(value->entry)(*env)->DeleteGlobalRef(env,value->entry);kl_host_callbacks_release(env,value->bindings);for(int i=0;i<3;i++)free(value->paths[i]);free(value);
}
jlong klj_native_activity_create(JNIEnv *env,jclass type,jlong image,jstring entry,jobject object,jobjectArray paths,jint sdk,jbyteArray saved){
    if(!installed||!object||!entry||!paths||(*env)->GetArrayLength(env,paths)!=3||sdk<1){fail(env,"NativeActivity is not configured");return 0;}
    pthread_mutex_lock(&gate);int occupied=current!=NULL;pthread_mutex_unlock(&gate);if(occupied){fail(env,"A native Activity already owns this process");return 0;}
    const char *name=(*env)->GetStringUTFChars(env,entry,NULL);if(!name)return 0;
    void (*create)(kl_ANativeActivity*,void*,size_t)=klj_guest_image_symbol(image,name);(*env)->ReleaseStringUTFChars(env,entry,name);
    if(!create){fail(env,"NativeActivity entry point was not found in its loaded library");return 0;}
    Activity *value=calloc(1,sizeof *value);if(!value){fail(env,"NativeActivity allocation failed");return 0;}
    value->owner=pthread_self();value->image=image;
    for(int i=0;i<3&&!(*env)->ExceptionCheck(env);i++){jbyteArray path=(*env)->GetObjectArrayElement(env,paths,i);if(!(*env)->ExceptionCheck(env))value->paths[i]=copy_path(env,path);if(path)(*env)->DeleteLocalRef(env,path);}
    if(!value->paths[0]||!value->paths[1])fail(env,"NativeActivity requires application data paths");
    if(!(*env)->ExceptionCheck(env)){value->bindings=kl_host_callbacks_create(env);if(value->bindings){KLHost_android_app_NativeActivity owner=kl_host_bind_KLHost_android_app_NativeActivity(env,value->bindings,object);if(owner.object)value->object=(*env)->NewGlobalRef(env,owner.object);}}
    if(!(*env)->ExceptionCheck(env)){value->entry=(*env)->NewGlobalRef(env,type);value->pump=(*env)->GetStaticMethodID(env,type,"pumpActivity","(J)V");}
    if(!(*env)->ExceptionCheck(env)&&(*env)->GetJavaVM(env,&value->vm)!=JNI_OK)fail(env,"NativeActivity Java VM is unavailable");
    if((*env)->ExceptionCheck(env)||!value->object){dispose(env,value);return 0;}
    JNIEnv *guest_env=NULL;JavaVM *guest_vm=NULL;
    value->provider_owned=klj_native_image_jni_context(env,image,&guest_env,&guest_vm);
    if(value->provider_owned&&((*env)->ExceptionCheck(env)||!guest_env||!guest_vm)){dispose(env,value);return 0;}
    if(!value->provider_owned){guest_env=klj_guest_env(env);guest_vm=klj_guest_vm(value->vm);}
    size_t size=saved?(size_t)(*env)->GetArrayLength(env,saved):0;void *data=NULL;
    if(size){data=malloc(size);if(!data){dispose(env,value);fail(env,"NativeActivity saved state allocation failed");return 0;}(*env)->GetByteArrayRegion(env,saved,0,(jsize)size,data);}
    if((*env)->ExceptionCheck(env)){free(data);dispose(env,value);return 0;}
    jclass activity_type=(*env)->GetObjectClass(env,object);
    jmethodID get_assets=activity_type?(*env)->GetMethodID(env,activity_type,"getAssets","()Landroid/content/res/AssetManager;"):NULL;
    jobject assets=get_assets?(*env)->CallObjectMethod(env,object,get_assets):NULL;
    void *native_assets=assets?asset_manager(env,assets):NULL;
    if(assets)(*env)->DeleteLocalRef(env,assets);if(activity_type)(*env)->DeleteLocalRef(env,activity_type);
    if(!native_assets||(*env)->ExceptionCheck(env)){free(data);dispose(env,value);if(!(*env)->ExceptionCheck(env))fail(env,"NativeActivity asset manager is unavailable");return 0;}
    value->activity=(kl_ANativeActivity){.callbacks=&value->callbacks,.vm=guest_vm,.env=guest_env,.clazz=value->object,.internalDataPath=value->paths[0],.externalDataPath=value->paths[1],.sdkVersion=sdk,.assetManager=native_assets,.obbPath=value->paths[2]};
    prepare_looper();pthread_mutex_lock(&gate);
    if(current){pthread_mutex_unlock(&gate);free(data);dispose(env,value);fail(env,"A native Activity already owns this process");return 0;}
    current=value;pthread_mutex_unlock(&gate);
    invoke_create(env,value,create,data,size);free(data);
    if((*env)->ExceptionCheck(env)){
        jthrowable first=(*env)->ExceptionOccurred(env);(*env)->ExceptionClear(env);
        if(value->callbacks.onDestroy)ACTIVITY_CALL(value,value->callbacks.onDestroy,0,0,value->callbacks.onDestroy(&value->activity));
        if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
        dispose(env,value);(*env)->Throw(env,first);(*env)->DeleteLocalRef(env,first);return 0;
    }
    return (jlong)(uintptr_t)value;
}
jbyteArray klj_native_activity_event(JNIEnv *env,jclass type,jlong handle,jint event,jlong argument){
    pthread_mutex_lock(&gate);Activity *value=current;int valid=value&&(uintptr_t)value==(uintptr_t)handle&&pthread_equal(value->owner,pthread_self());pthread_mutex_unlock(&gate);
    if(!valid){fail(env,"NativeActivity callback has no matching owner");return NULL;}
    kl_ANativeActivity *a=&value->activity;kl_ANativeActivityCallbacks *c=&value->callbacks;void *window=(void*)(uintptr_t)argument;
    switch(event){
        case 1:if(c->onStart)ACTIVITY_CALL(value,c->onStart,0,0,c->onStart(a));break;
        case 2:if(c->onResume)ACTIVITY_CALL(value,c->onResume,0,0,c->onResume(a));break;
        case 3:if(c->onPause)ACTIVITY_CALL(value,c->onPause,0,0,c->onPause(a));break;
        case 4:if(c->onStop)ACTIVITY_CALL(value,c->onStop,0,0,c->onStop(a));break;
        case 5:{
            size_t size=0;void *volatile data=NULL;jbyteArray out=NULL;
            if(c->onSaveInstanceState){uint64_t result=0;
                if(!klj_native_activity_callback(env,value->image,(const void*)c->onSaveInstanceState,(uintptr_t)a,(uintptr_t)&size,0,&result))
                    KLJ_INVOKE_GUEST(env,data=c->onSaveInstanceState(a,&size));
                else data=(void*)(uintptr_t)result;
            }
            if(!(*env)->ExceptionCheck(env)){
                if(size>INT_MAX||(!data&&size))fail(env,"NativeActivity returned invalid saved state");
                else if(data){out=(*env)->NewByteArray(env,(jsize)size);if(out&&size)(*env)->SetByteArrayRegion(env,out,0,(jsize)size,data);}
            }
            release_saved(env,value,(void*)data);return out;
        }
        case 6:if(c->onDestroy)ACTIVITY_CALL(value,c->onDestroy,0,0,c->onDestroy(a));dispose(env,value);break;
        case 7:if(c->onWindowFocusChanged)ACTIVITY_CALL(value,c->onWindowFocusChanged,argument!=0,0,c->onWindowFocusChanged(a,argument!=0));break;
        case 8:if(!window||value->window){fail(env,"NativeActivity window creation is out of order");break;}value->window=window;if(c->onNativeWindowCreated)ACTIVITY_CALL(value,c->onNativeWindowCreated,window,0,c->onNativeWindowCreated(a,window));break;
        case 9:if(window!=value->window||!window){fail(env,"NativeActivity window resize is out of order");break;}if(c->onNativeWindowResized)ACTIVITY_CALL(value,c->onNativeWindowResized,window,0,c->onNativeWindowResized(a,window));break;
        case 10:if(window!=value->window||!window){fail(env,"NativeActivity window redraw is out of order");break;}if(c->onNativeWindowRedrawNeeded)ACTIVITY_CALL(value,c->onNativeWindowRedrawNeeded,window,0,c->onNativeWindowRedrawNeeded(a,window));break;
        case 11:if(window!=value->window||!window){fail(env,"NativeActivity window destruction is out of order");break;}value->window=NULL;if(c->onNativeWindowDestroyed)ACTIVITY_CALL(value,c->onNativeWindowDestroyed,window,0,c->onNativeWindowDestroyed(a,window));break;
        case 12:if(c->onConfigurationChanged)ACTIVITY_CALL(value,c->onConfigurationChanged,0,0,c->onConfigurationChanged(a));break;
        case 13:if(c->onLowMemory)ACTIVITY_CALL(value,c->onLowMemory,0,0,c->onLowMemory(a));break;
        case 14:if(c->onInputQueueCreated)ACTIVITY_CALL(value,c->onInputQueueCreated,window,0,c->onInputQueueCreated(a,window));break;
        case 15:if(c->onInputQueueDestroyed)ACTIVITY_CALL(value,c->onInputQueueDestroyed,window,0,c->onInputQueueDestroyed(a,window));break;
        default:fail(env,"Unknown NativeActivity callback");
    }
    return NULL;
}
void klj_native_activity_content_rect(JNIEnv *env,jclass type,jlong handle,jint x,jint y,jint width,jint height){
    pthread_mutex_lock(&gate);Activity *value=current;int valid=value&&(uintptr_t)value==(uintptr_t)handle&&pthread_equal(value->owner,pthread_self());pthread_mutex_unlock(&gate);
    if(!valid){fail(env,"NativeActivity content rectangle has no matching owner");return;}
    if(value->callbacks.onContentRectChanged){int32_t rect[4]={x,y,(int32_t)((int64_t)x+width),(int32_t)((int64_t)y+height)};ACTIVITY_CALL(value,value->callbacks.onContentRectChanged,rect,0,value->callbacks.onContentRectChanged(&value->activity,rect));}
}
void klj_native_activity_dispatch(JNIEnv *env,jclass type,jlong handle){
    pthread_mutex_lock(&gate);Activity *value=current;int valid=value&&(uintptr_t)value==(uintptr_t)handle&&pthread_equal(value->owner,pthread_self());pthread_mutex_unlock(&gate);
    if(!valid){fail(env,"NativeActivity looper requires its owning thread");return;}
    int result=0;KLJ_INVOKE_GUEST(env,result=pump_looper(0));if((*env)->ExceptionCheck(env))return;
    if(result==-4)fail(env,"NativeActivity looper polling failed");
}
int klj_native_activity_pump(JNIEnv *env){
    pthread_mutex_lock(&gate);Activity *value=current;int valid=value&&pthread_equal(value->owner,pthread_self());pthread_mutex_unlock(&gate);
    if(!value)return 1;if(!valid){fail(env,"NativeActivity looper requires its owning thread");return 0;}
    if((*env)->ExceptionCheck(env))return 0;
    // FD callbacks need the same FindClass namespace as onCreate and lifecycle
    // callbacks. Enter the cached guest-parented native declaration first.
    jclass entry=(*env)->NewLocalRef(env,value->entry);if(!entry)return 0;
    jvalue argument={.j=(jlong)(uintptr_t)value};
    (*env)->CallStaticVoidMethodA(env,entry,value->pump,&argument);
    int ok=!(*env)->ExceptionCheck(env);(*env)->DeleteLocalRef(env,entry);return ok;
}
