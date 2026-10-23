#pragma once
#include <jni.h>
#include <stddef.h>
#include <stdint.h>
#include "call-packet.h"

/* One explicitly initialized application-image executor per JVM. The existing
 * native framework suppliers retain their platform bindings. Installation is
 * complete before the first application image; filenames do not select an
 * executor. Each provider retains its opaque handles for the VM lifetime. */
#define KLJ_NATIVE_IMAGE_PROVIDER_ABI 1u
typedef struct {
    uint32_t size,version;
    void *owner;
    jlong (*load)(void *,JNIEnv *,jclass,const char *,const char *,jobject,jboolean);
    int (*owns)(void *,jlong);
    void *(*symbol)(void *,jlong,const char *,char *,size_t);
    void *(*native_abi)(void *,jlong,void *,const char *,char *,size_t);
    jint (*register_exports)(void *,JNIEnv *,jlong,jclass,const JNINativeMethod *,jint);
    int (*initialize)(void *,JNIEnv *,jlong);
    int (*construct)(void *,JNIEnv *,jlong);
    void (*reject)(void *,jlong);
    /* Raw non-JNI callback: x0 is an original argument, never a JNIEnv.
     * The provider validates the retained image and current callback target. */
    int64_t (*invoke)(void *,JNIEnv *,jlong,KljCallPacket *);
    int64_t (*jni_context)(void *,JNIEnv *,jlong,JNIEnv **,JavaVM **);
} KljNativeImageProvider;
int klj_native_image_provider_install(const KljNativeImageProvider *,char *,size_t);
/* Invoked synchronously on the actual Java loading thread before constructors.
 * JNI exceptional returns preserve the first Throwable. */
int klj_native_image_prepare(JNIEnv *,jobject,jlong);
jlong klj_load_prepared(JNIEnv *,jclass,jstring,jstring,jstring,jobject);

/* Zero means a legacy image: the caller executes its unchanged native call.
 * One means provider-owned, including failure. Never fall back after one.
 * packet.status and the original pending Java exception carry failure. */
int klj_native_image_invoke(JNIEnv *,jlong,KljCallPacket *);
/* Same handled/not-owned convention. These are genuine JNI carriers only;
 * native windows, queues and assets retain their separate supplier ownership. */
int klj_native_image_jni_context(JNIEnv *,jlong,JNIEnv **,JavaVM **);
