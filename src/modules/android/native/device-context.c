#include "device-context.h"
#include "jni-invocation.h"
#include "kl_jni.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(KLHostCallbacks *) callbacks;
extern void klj_enter_guest_thread(void);
extern void kl_ndk_set_assets_dir(const char *);
static const char *assets_directory,*files_directory;
void kl_jni_set_assets_dir(const char *directory){assets_directory=directory;kl_ndk_set_assets_dir(directory);}
void kl_jni_set_files_dir(const char *directory){files_directory=directory;}
const char *kl_jni_assets_dir(void){return assets_directory;}
const char *kl_jni_files_dir(void){return files_directory;}
JNIEnv *klj_device_environment(void){
    JavaVM *vm=klj_jni_actual_vm();
    JNIEnv *env=NULL;if(!vm)return NULL;
    jint status=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(status==JNI_OK)return env;
    if(status!=JNI_EDETACHED)return NULL;
    klj_enter_guest_thread();
    return (*vm)->AttachCurrentThreadAsDaemon(vm,(void**)&env,NULL)==JNI_OK?env:NULL;
}
KLHostCallbacks *klj_device_callbacks(void){
    KLHostCallbacks *value=atomic_load_explicit(&callbacks,memory_order_acquire);
    if(value)return value;
    JNIEnv *env=klj_device_environment();if(!env)return NULL;
    KLHostCallbacks *made=kl_host_callbacks_create(env);if(!made)return NULL;
    if(!atomic_compare_exchange_strong_explicit(&callbacks,&value,made,memory_order_release,memory_order_acquire)){
        kl_host_callbacks_release(env,made);return value;
    }
    return made;
}

/* The names below are Klepton's native helper ABI. All Java behavior is
 * supplied by the actual VM and original framework, never a synthetic class
 * registry. Values retained by native engines have process lifetime. */
typedef struct DeviceString {char *key,*value;struct DeviceString *next;} DeviceString;
static DeviceString *strings;
static pthread_mutex_t strings_lock=PTHREAD_MUTEX_INITIALIZER;
static const char *cached_string(const char *key){
    const char *value=NULL;pthread_mutex_lock(&strings_lock);
    for(DeviceString *s=strings;s;s=s->next)if(!strcmp(s->key,key)){value=s->value;break;}
    pthread_mutex_unlock(&strings_lock);return value;
}
static const char *retain_string(JNIEnv *env,const char *key,jstring text){
    if(!text)return NULL;
    const char *utf=(*env)->GetStringUTFChars(env,text,NULL);if(!utf)return NULL;
    DeviceString *made=calloc(1,sizeof(*made));
    if(made){made->key=strdup(key);made->value=strdup(utf);}
    (*env)->ReleaseStringUTFChars(env,text,utf);
    if(!made||!made->key||!made->value){if(made){free(made->key);free(made->value);free(made);}return NULL;}
    pthread_mutex_lock(&strings_lock);
    const char *result=NULL;
    for(DeviceString *s=strings;s;s=s->next)if(!strcmp(s->key,key)){result=s->value;break;}
    if(!result){made->next=strings;strings=made;result=made->value;made=NULL;}
    pthread_mutex_unlock(&strings_lock);
    if(made){free(made->key);free(made->value);free(made);}
    return result;
}
static const char *device_string(const char *key){
    if(!key)return NULL;
    const char *cached=cached_string(key);if(cached)return cached;
    JNIEnv *env=klj_device_environment();if(!env||(*env)->ExceptionCheck(env))return NULL;
    KLHostCallbacks *cb=klj_device_callbacks();if(!cb||!klj_jni_before_java(env))return NULL;
    jstring name=(*env)->NewStringUTF(env,key);
    jstring value=name?(jstring)kl_host_device_string(env,cb,(KLHost_java_lang_String){name}).object:NULL;
    const char *result=value?retain_string(env,key,value):NULL;
    if(value)(*env)->DeleteLocalRef(env,value);
    if(name)(*env)->DeleteLocalRef(env,name);
    klj_jni_after_java(env);return result;
}
const char *kl_driver_target_name(void){return device_string("package");}
const char *klj_guest_package(void){return device_string("package");}
const char *kl_jni_guest_package(void){return klj_guest_package();}
void kl_jni_guest_version(long *code,const char **name){
    if(name)*name=device_string("versionName");
    if(!code)return;
    *code=0;JNIEnv *env=klj_device_environment();if(!env||(*env)->ExceptionCheck(env))return;
    KLHostCallbacks *cb=klj_device_callbacks();if(!cb||!klj_jni_before_java(env))return;
    *code=(long)kl_host_device_version(env,cb);klj_jni_after_java(env);
}
static void copy_string(JNIEnv *env,jstring text,char *output,size_t size){
    if(!output||!size)return;output[0]=0;if(!text)return;
    const char *utf=(*env)->GetStringUTFChars(env,text,NULL);if(!utf)return;
    snprintf(output,size,"%s",utf);(*env)->ReleaseStringUTFChars(env,text,utf);
}
void kl_jni_locale_parts(char *language,size_t language_size,char *country,size_t country_size){
    if(language&&language_size)language[0]=0;if(country&&country_size)country[0]=0;
    JNIEnv *env=klj_device_environment();if(!env||(*env)->ExceptionCheck(env))return;
    KLHostCallbacks *cb=klj_device_callbacks();if(!cb||!klj_jni_before_java(env))return;
    jobjectArray locale=kl_host_device_locale(env,cb);
    if(locale){
        jstring lang=(*env)->GetObjectArrayElement(env,locale,0);
        jstring region=(*env)->ExceptionCheck(env)?NULL:(*env)->GetObjectArrayElement(env,locale,1);
        if(!(*env)->ExceptionCheck(env)){copy_string(env,lang,language,language_size);copy_string(env,region,country,country_size);}
        if(lang)(*env)->DeleteLocalRef(env,lang);if(region)(*env)->DeleteLocalRef(env,region);
        (*env)->DeleteLocalRef(env,locale);
    }
    klj_jni_after_java(env);
}
int kl_jni_low_memory(void){
    JNIEnv *env=klj_device_environment();if(!env||(*env)->ExceptionCheck(env))return 0;
    KLHostCallbacks *cb=klj_device_callbacks();if(!cb||!klj_jni_before_java(env))return 0;
    int result=kl_host_device_low_memory(env,cb);klj_jni_after_java(env);return result;
}
void *kl_jni_vm(void){JavaVM *vm=NULL;jsize count=0;return klj_jni_get_created_vms(&vm,1,&count)==JNI_OK&&count==1?vm:NULL;}
void kl_jni_report(FILE *output){
    JavaVM *vm=klj_jni_actual_vm();JNIEnv *env=NULL;
    jint state=vm?(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6):JNI_ERR;
    fprintf(output,"[android-jvm] VM %s, current thread %s\n",vm?"present":"absent",state==JNI_OK?"attached":"detached");
}
void klj_device_context_release(JNIEnv *env){
    kl_host_callbacks_release(env,atomic_exchange_explicit(&callbacks,NULL,memory_order_acq_rel));
    pthread_mutex_lock(&strings_lock);
    while(strings){DeviceString *next=strings->next;free(strings->key);free(strings->value);free(strings);strings=next;}
    pthread_mutex_unlock(&strings_lock);
    assets_directory=files_directory=NULL;
}
