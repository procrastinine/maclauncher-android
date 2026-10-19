#include "display-adapter.h"
#include "display-snapshot.h"
#include <stdlib.h>
#include <stdint.h>
extern void klj_ndk_choreographer_refresh_period(int64_t);
struct KLDisplayAdapter{JNIEnv* owner;jclass display,manager;jmethodID publish,remove,inflight;int stopped,retired;};
static void fail(JNIEnv* e,const char* text){if((*e)->ExceptionCheck(e))return;jclass c=(*e)->FindClass(e,"java/lang/IllegalStateException");if(c){(*e)->ThrowNew(e,c,text);(*e)->DeleteLocalRef(e,c);}}
static int owns(JNIEnv* e,KLDisplayAdapter* s){if(s&&s->owner==e)return 1;fail(e,"Display adapter owner is unavailable");return 0;}
static jclass global(JNIEnv* e,const char* name){jclass c=(*e)->FindClass(e,name);if(!c)return NULL;jclass g=(*e)->NewGlobalRef(e,c);(*e)->DeleteLocalRef(e,c);return g;}
static void release(JNIEnv* e,KLDisplayAdapter* s){if(s->display)(*e)->DeleteGlobalRef(e,s->display);if(s->manager)(*e)->DeleteGlobalRef(e,s->manager);free(s);}
static void capture(JNIEnv* e,jthrowable* first){jthrowable next=(*e)->ExceptionOccurred(e);if(!next)return;(*e)->ExceptionClear(e);if(!*first){*first=next;return;}if(!(*e)->IsSameObject(e,*first,next)){jclass c=(*e)->GetObjectClass(e,*first);jmethodID m=c?(*e)->GetMethodID(e,c,"addSuppressed","(Ljava/lang/Throwable;)V"):NULL;if(m)(*e)->CallVoidMethod(e,*first,m,next);if((*e)->ExceptionCheck(e))(*e)->ExceptionClear(e);if(c)(*e)->DeleteLocalRef(e,c);}(*e)->DeleteLocalRef(e,next);}
KLDisplayAdapter* kl_display_adapter_create(JNIEnv* e){
    if((*e)->ExceptionCheck(e))return NULL;KLDisplayAdapter* s=calloc(1,sizeof(*s));if(!s){fail(e,"Display adapter allocation failed");return NULL;}s->owner=e;
    s->display=global(e,"maclauncher/android/framework/HostDisplay");if(!s->display)goto bad;
    s->manager=global(e,"maclauncher/android/framework/HostDisplay");if(!s->manager)goto bad;
    s->publish=(*e)->GetStaticMethodID(e,s->display,"hostPublish","(JIIFF)Z");if(!s->publish)goto bad;
    s->remove=(*e)->GetStaticMethodID(e,s->display,"hostDetachOutput","()V");if(!s->remove)goto bad;
    s->inflight=(*e)->GetStaticMethodID(e,s->manager,"hostHasInFlightCallbacks","()Z");if(!s->inflight)goto bad;
    return s;
bad:release(e,s);return NULL;
}
int kl_display_adapter_update(JNIEnv* e,KLDisplayAdapter* s,void* window,int width,int height){
    if(!owns(e,s)||(*e)->ExceptionCheck(e))return 0;if(s->stopped){fail(e,"Display adapter has stopped");return 0;}
    if(width<1||height<1){fail(e,"Committed Android surface extent is unavailable");return 0;}
    KLDisplaySnapshot value;char error[256];if(!kl_display_snapshot_read(window,&value,error,sizeof(error))){fail(e,error);return 0;}
    (*e)->CallStaticBooleanMethod(e,s->display,s->publish,(jlong)value.screenId,(jint)width,(jint)height,(jfloat)value.refreshRate,(jfloat)(width/value.windowWidth));
    if(!(*e)->ExceptionCheck(e))klj_ndk_choreographer_refresh_period((int64_t)(1000000000.0/value.refreshRate+0.5));
    return !(*e)->ExceptionCheck(e);
}
int kl_display_adapter_stop(JNIEnv* e,KLDisplayAdapter* s){
    if(!owns(e,s))return 0;if(s->retired)return 1;jthrowable first=NULL;capture(e,&first);
    s->stopped=1;
    jboolean active=(*e)->CallStaticBooleanMethod(e,s->manager,s->inflight);if((*e)->ExceptionCheck(e)){capture(e,&first);active=JNI_TRUE;}s->retired=s->stopped&&!active;
    if(first){(*e)->Throw(e,first);(*e)->DeleteLocalRef(e,first);}return s->retired;
}
int kl_display_adapter_destroy(JNIEnv* e,KLDisplayAdapter* s){if(!s)return 1;if(!owns(e,s))return 0;if(!s->retired){fail(e,"Display callbacks must retire before native adapter destruction");return 0;}jthrowable first=NULL;capture(e,&first);(*e)->CallStaticVoidMethod(e,s->display,s->remove);int removed=!(*e)->ExceptionCheck(e);capture(e,&first);if(removed)release(e,s);if(first){(*e)->Throw(e,first);(*e)->DeleteLocalRef(e,first);}return removed;}
