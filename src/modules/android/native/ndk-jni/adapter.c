#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ndk-jni-imports.h"

extern int klj_guest_address(const void *);
extern JNIEnv *klj_native_env(JNIEnv *);
extern void *klj_native_env_abi(void *,void *(*)(void *),unsigned,char *,size_t);
static void *environment(void *value){return klj_native_env(value);}

void *klj_native_jni_import(const char *name,void *target) {
    if(!target)return NULL;
    for(size_t i=0;i<sizeof(ndk_jni_imports)/sizeof(*ndk_jni_imports);i++) {
        if(strcmp(name,ndk_jni_imports[i].name))continue;
        // Publisher-owned ELF implementations retain their Android environment.
        if(klj_guest_address(target))return target;
        char error[256];
        void *entry=klj_native_env_abi(target,environment,ndk_jni_imports[i].argument,error,sizeof(error));
        if(!entry)fprintf(stderr,"Android native environment adapter: %s\n",error);
        return entry;
    }
    return target;
}
