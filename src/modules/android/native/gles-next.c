#include <jni.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdatomic.h>
#include "gles-common.h"
#include "gles-diagnostics-support.h"
static void (*uniform1iv)(int,int,const int*);
static void (*activeTexture)(unsigned);
static void*(*graphicsSymbol)(const char*);
static atomic_int debugRecords;
static void fail(JNIEnv*e,const char*type,const char*message){jclass c=(*e)->FindClass(e,type);if(c){(*e)->ThrowNew(e,c,message);(*e)->DeleteLocalRef(e,c);}}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NextGLES_configure(JNIEnv*e,jclass c,jstring path){
    (void)c;if(!path){fail(e,"java/lang/IllegalStateException","GLES library is not configured");return;}
    const char*p=(*e)->GetStringUTFChars(e,path,NULL);if(!p)return;
    void*handle=dlopen(p,RTLD_NOW|RTLD_LOCAL);(*e)->ReleaseStringUTFChars(e,path,p);
    if(!handle){fail(e,"java/lang/UnsatisfiedLinkError","Unable to open the configured GLES host");return;}
    void*(*resolve)(const char*)=(void*(*)(const char*))dlsym(handle,"klj_graphics_symbol");
    void*address=resolve?resolve("glUniform1iv"):NULL;
    void*active=resolve?resolve("glActiveTexture"):NULL;
    // The configured graphics host is already owned by the JVM. Drop this
    // temporary lookup reference; the host stays loaded for the process.
    dlclose(handle);
    if(!address||!active){fail(e,"java/lang/UnsatisfiedLinkError","The configured GLES host does not export the required uniform/texture functions");return;}
    uniform1iv=(void(*)(int,int,const int*))address;
    activeTexture=(void(*)(unsigned))active;
    graphicsSymbol=resolve;
    klj_next_set_resolver(resolve);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NextGLES_activeTexture(JNIEnv*e,jclass c,jint texture){(void)c;if(!activeTexture){fail(e,"java/lang/IllegalStateException","GLES texture function is not configured");return;}activeTexture((unsigned)texture);}
static void debugMessage(unsigned source,unsigned type,unsigned id,unsigned severity,int length,const char*message,const void*user){
    (void)source;(void)severity;(void)user;
    if(type!=0x824C||!message||length<0)return;
    int observed=atomic_load(&debugRecords);
    do{if(observed>=32)return;}while(!atomic_compare_exchange_weak(&debugRecords,&observed,observed+1));
    fprintf(stderr,"[gles-debug] error %u: %.*s\n",id,length<512?length:512,message);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NextGLES_enableDiagnostics(JNIEnv*e,jclass c){
    (void)c;if(!graphicsSymbol){fail(e,"java/lang/IllegalStateException","GLES functions are not configured");return;}
    const unsigned char*(*getString)(unsigned)=(const unsigned char*(*)(unsigned))graphicsSymbol("glGetString");
    const char *version=getString?(const char*)getString(0x1F02):NULL;
    if(!getString||!klj_next_debug_supported(version,(const char*)getString(0x1F03))){
        fprintf(stderr,"[gles-debug] Current context does not expose GLES 3.2 or KHR_debug; diagnostic callback skipped\n");return;
    }
    typedef void (*Callback)(unsigned,unsigned,unsigned,unsigned,int,const char*,const void*);
    const char *entry=klj_next_debug_core(version)?"glDebugMessageCallback":"glDebugMessageCallbackKHR";
    void(*callback)(Callback,const void*)=(void(*)(Callback,const void*))graphicsSymbol(entry);
    void(*enable)(unsigned)=(void(*)(unsigned))graphicsSymbol("glEnable");
    if(!callback||!enable){fail(e,"java/lang/UnsupportedOperationException","The current GLES library has no debug callback");return;}
    callback(debugMessage,NULL);enable(0x92E0);enable(0x8242);
}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NextGLES_nUniform1iv(JNIEnv*e,jclass c,jint location,jint count,jobject values){
    (void)c;if(!uniform1iv){fail(e,"java/lang/IllegalStateException","GLES uniform function is not configured");return;}
    if(!values||count<0){fail(e,"java/lang/IllegalArgumentException","Invalid GLES uniform buffer");return;}
    jclass type=(*e)->GetObjectClass(e,values);if(!type)return;
    jmethodID position=(*e)->GetMethodID(e,type,"position","()I"),remaining=(*e)->GetMethodID(e,type,"remaining","()I");
    (*e)->DeleteLocalRef(e,type);if((*e)->ExceptionCheck(e))return;
    jint pos=(*e)->CallIntMethod(e,values,position);if((*e)->ExceptionCheck(e))return;
    jint available=(*e)->CallIntMethod(e,values,remaining);if((*e)->ExceptionCheck(e))return;
    jint*p=(*e)->GetDirectBufferAddress(e,values);jlong capacity=(*e)->GetDirectBufferCapacity(e,values);
    if(pos<0||available<0||count>available||capacity<(jlong)pos+available||(!p&&count)){fail(e,"java/lang/IllegalArgumentException","GLES uniform buffer range is invalid");return;}
    uniform1iv(location,count,p?p+pos:NULL);
}
