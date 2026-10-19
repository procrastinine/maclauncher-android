#include <jni.h>

/* Android's public EventLog keeps its original native declarations and decoder.
 * Darwin has no Android logd; bounded application-lifetime storage is its owner. */
static jint event_write(JNIEnv *env,jint tag,jvalue value,const char *signature){
    jclass owner=(*env)->FindClass(env,"maclauncher/android/framework/HostEventLog");if(!owner)return -1;
    jmethodID method=(*env)->GetStaticMethodID(env,owner,"write",signature);jint result=-1;
    if(method){jvalue values[2]={{.i=tag},value};result=(*env)->CallStaticIntMethodA(env,owner,method,values);}
    (*env)->DeleteLocalRef(env,owner);return result;
}
JNIEXPORT jint JNICALL Java_android_util_EventLog_writeEvent__II(JNIEnv *env,jclass type,jint tag,jint value){(void)type;return event_write(env,tag,(jvalue){.i=value},"(II)I");}
JNIEXPORT jint JNICALL Java_android_util_EventLog_writeEvent__IJ(JNIEnv *env,jclass type,jint tag,jlong value){(void)type;return event_write(env,tag,(jvalue){.j=value},"(IJ)I");}
JNIEXPORT jint JNICALL Java_android_util_EventLog_writeEvent__IF(JNIEnv *env,jclass type,jint tag,jfloat value){(void)type;return event_write(env,tag,(jvalue){.f=value},"(IF)I");}
JNIEXPORT jint JNICALL Java_android_util_EventLog_writeEvent__ILjava_lang_String_2(JNIEnv *env,jclass type,jint tag,jstring value){(void)type;return event_write(env,tag,(jvalue){.l=value},"(ILjava/lang/Object;)I");}
JNIEXPORT jint JNICALL Java_android_util_EventLog_writeEvent__I_3Ljava_lang_Object_2(JNIEnv *env,jclass type,jint tag,jobjectArray value){(void)type;return event_write(env,tag,(jvalue){.l=value},"(ILjava/lang/Object;)I");}
static void read_events(JNIEnv *env,jintArray tags,jlong since,jobject destination){
    jclass owner=(*env)->FindClass(env,"maclauncher/android/framework/HostEventLog");if(!owner)return;
    jmethodID method=(*env)->GetStaticMethodID(env,owner,"read","([IJLjava/util/Collection;)V");
    if(method)(*env)->CallStaticVoidMethod(env,owner,method,tags,since,destination);
    (*env)->DeleteLocalRef(env,owner);
}
JNIEXPORT void JNICALL Java_android_util_EventLog_readEvents(JNIEnv *env,jclass type,jintArray tags,jobject destination){(void)type;read_events(env,tags,0,destination);}
JNIEXPORT void JNICALL Java_android_util_EventLog_readEventsOnWrapping(JNIEnv *env,jclass type,jintArray tags,jlong since,jobject destination){(void)type;read_events(env,tags,since,destination);}
JNIEXPORT jlong JNICALL Java_com_android_internal_os_DebugStore_beginEventNative(JNIEnv *env,jclass type,jstring name,jobject attributes){
    (void)type;jclass owner=(*env)->FindClass(env,"maclauncher/android/framework/HostEventLog");if(!owner)return 0;
    jmethodID method=(*env)->GetStaticMethodID(env,owner,"begin","(Ljava/lang/String;Ljava/util/List;)J");jlong result=0;
    if(method)result=(*env)->CallStaticLongMethod(env,owner,method,name,attributes);(*env)->DeleteLocalRef(env,owner);return result;
}
JNIEXPORT void JNICALL Java_com_android_internal_os_DebugStore_endEventNative(JNIEnv *env,jclass type,jlong id,jobject attributes){
    (void)type;jclass owner=(*env)->FindClass(env,"maclauncher/android/framework/HostEventLog");if(!owner)return;
    jmethodID method=(*env)->GetStaticMethodID(env,owner,"end","(JLjava/util/List;)V");
    if(method)(*env)->CallStaticVoidMethod(env,owner,method,id,attributes);(*env)->DeleteLocalRef(env,owner);
}
JNIEXPORT void JNICALL Java_com_android_internal_os_DebugStore_recordEventNative(JNIEnv *env,jclass type,jstring name,jobject attributes){
    (void)type;jclass owner=(*env)->FindClass(env,"maclauncher/android/framework/HostEventLog");if(!owner)return;
    jmethodID method=(*env)->GetStaticMethodID(env,owner,"instant","(Ljava/lang/String;Ljava/util/List;)V");
    if(method)(*env)->CallStaticVoidMethod(env,owner,method,name,attributes);(*env)->DeleteLocalRef(env,owner);
}
