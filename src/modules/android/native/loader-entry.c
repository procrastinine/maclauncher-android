#include <jni.h>
#include "native-image-provider.h"
extern jlong Java_maclauncher_android_bridge_NativeBinding_load(JNIEnv*,jclass,jstring,jstring,jstring);
extern void klj_initialize_guest(JNIEnv*,jlong);
extern void klj_construct_guest(JNIEnv*,jlong);
extern void klj_reject_guest(jlong);
extern jlong klj_native_activity_create(JNIEnv*,jclass,jlong,jstring,jobject,jobjectArray,jint,jbyteArray);
extern jbyteArray klj_native_activity_event(JNIEnv*,jclass,jlong,jint,jlong);
extern void klj_native_activity_dispatch(JNIEnv*,jclass,jlong);
extern void klj_native_activity_content_rect(JNIEnv*,jclass,jlong,jint,jint,jint,jint);
_Thread_local int klj_skip_guest_onload;
static jlong load_entry(JNIEnv *env,jclass cls,jstring runtime,jstring file,jstring soname,jboolean initialize){int previous=klj_skip_guest_onload;klj_skip_guest_onload=!initialize;jlong result=Java_maclauncher_android_bridge_NativeBinding_load(env,cls,runtime,file,soname);klj_skip_guest_onload=previous;return result;}
static jlong load_prepared_entry(JNIEnv *env,jclass cls,jstring runtime,jstring file,jstring soname,jobject preparation){return klj_load_prepared(env,cls,runtime,file,soname,preparation);}
static void initialize_entry(JNIEnv *env,jclass cls,jlong image){klj_initialize_guest(env,image);}
static void construct_entry(JNIEnv *env,jclass cls,jlong image){klj_construct_guest(env,image);}
static void reject_entry(JNIEnv *env,jclass cls,jlong image){klj_reject_guest(image);}
JNIEXPORT void JNICALL Java_maclauncher_android_bridge_NativeBinding_registerEntry(JNIEnv *env,jclass ignored,jclass entry){
    JNINativeMethod methods[]={{"load","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)J",(void*)load_entry},{"loadPrepared","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/util/function/LongConsumer;)J",(void*)load_prepared_entry},{"initialize","(J)V",(void*)initialize_entry},{"createActivity","(JLjava/lang/String;Landroid/app/NativeActivity;[[BI[B)J",(void*)klj_native_activity_create},{"activityEvent","(JIJ)[B",(void*)klj_native_activity_event},{"activityContentRect","(JIIII)V",(void*)klj_native_activity_content_rect},{"pumpActivity","(J)V",(void*)klj_native_activity_dispatch}};
    JNINativeMethod construct={"construct","(J)V",(void*)construct_entry};
    JNINativeMethod reject={"reject","(J)V",(void*)reject_entry};
    if((*env)->RegisterNatives(env,entry,methods,sizeof methods/sizeof methods[0])==JNI_OK && (*env)->RegisterNatives(env,entry,&construct,1)==JNI_OK)(*env)->RegisterNatives(env,entry,&reject,1);
}
