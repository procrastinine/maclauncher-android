#pragma once
#include <jni.h>
typedef struct KljRegistrationTransaction KljRegistrationTransaction;
/* Internal exported-method publication, not an interception of JNI calls.
 * stage accepts already converted host closures and never changes the VM.
 * The explicit per-image object can be shared by class-definition threads.
 * Its small state record remains alive with the image; staging references are
 * released by commit/discard. No class initialization is performed. */
KljRegistrationTransaction *klj_registration_txn_begin(JNIEnv *);
jint klj_registration_txn_stage_exports(JNIEnv *,KljRegistrationTransaction *,jclass,const JNINativeMethod *,jint);
/* Complete prevalidation precedes the first original RegisterNatives call.
 * Ordinary JDK21 registration cannot fail after valid native declarations are
 * established under standard schema-preserving redefinition. A replaced VM
 * registration function or external-agent failure is not an atomic API:
 * PARTIAL requires retaining the image/code, never unloading or discarding it.
 * The original pending Throwable is retained. */
enum { KLJ_REGISTRATION_COMMIT_PARTIAL=-2 };
jint klj_registration_txn_commit(JNIEnv *,KljRegistrationTransaction *);
jint klj_registration_txn_discard(JNIEnv *,KljRegistrationTransaction *);
