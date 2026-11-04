#include <jni.h>
#include <jvmti.h>
#include <dlfcn.h>
#include <stdio.h>

/* Attribute CheckJNI output to the native caller without suppressing it or
 * changing the VM's exception state. Other JNI operations remain untouched. */
static const char *(JNICALL *original_utf)(JNIEnv *,jstring,jboolean *);
static const char *JNICALL string_utf(JNIEnv *env,jstring value,jboolean *copy){
    Dl_info owner={0};
    dladdr(__builtin_return_address(0),&owner);
    printf("JNI-CALL-ORIGIN %s\n",owner.dli_fname?owner.dli_fname:"unknown");fflush(stdout);
    const char *result=original_utf(env,value,copy);
    printf("JNI-CALL-END\n");fflush(stdout);
    return result;
}
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved){
    (void)reserved;
    jvmtiEnv *ti=NULL;jniNativeInterface *table=NULL;
    if((*vm)->GetEnv(vm,(void **)&ti,JVMTI_VERSION_1_2)!=JNI_OK)return JNI_ERR;
    if((*ti)->GetJNIFunctionTable(ti,&table)!=JVMTI_ERROR_NONE)return JNI_ERR;
    original_utf=table->GetStringUTFChars;table->GetStringUTFChars=string_utf;
    jvmtiError result=(*ti)->SetJNIFunctionTable(ti,table);
    (*ti)->Deallocate(ti,(unsigned char *)table);
    (*ti)->DisposeEnvironment(ti);
    return result==JVMTI_ERROR_NONE?JNI_VERSION_1_6:JNI_ERR;
}
