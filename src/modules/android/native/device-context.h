#pragma once
#include <jni.h>
#include "host-callbacks.h"

/* Borrowed JVM references; thread retirement is owned by the common native
 * thread lifetime. These helpers do not create a VM or a Java object model. */
JNIEnv *klj_device_environment(void);
KLHostCallbacks *klj_device_callbacks(void);
void klj_device_context_release(JNIEnv *env);
const char *kl_jni_files_dir(void);
const char *kl_jni_assets_dir(void);
