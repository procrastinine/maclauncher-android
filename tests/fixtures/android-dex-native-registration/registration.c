#include <jni.h>
static jobjectArray JNICALL names(JNIEnv *env,jclass type,jobject cookie){
 (void)type;(void)cookie;
 jclass string=(*env)->FindClass(env,"java/lang/String");if(!string)return NULL;
 jstring name=(*env)->NewStringUTF(env,"fixture.Rebound");
 jobjectArray result=name?(*env)->NewObjectArray(env,1,string,name):NULL;
 if(name)(*env)->DeleteLocalRef(env,name);(*env)->DeleteLocalRef(env,string);return result;
}
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm,void *reserved){
 (void)reserved;JNIEnv *env=NULL;if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
 jclass type=(*env)->FindClass(env,"dalvik/system/DexFile");if(!type)return JNI_ERR;
 JNINativeMethod method={"getClassNameList","(Ljava/lang/Object;)[Ljava/lang/String;",(void*)names};
 jint result=(*env)->RegisterNatives(env,type,&method,1);(*env)->DeleteLocalRef(env,type);
 return result==JNI_OK?JNI_VERSION_1_6:JNI_ERR;
}
JNIEXPORT jint JNICALL Java_owned_Rebinder_unregister(JNIEnv *env,jclass ignored){
 (void)ignored;jclass type=(*env)->FindClass(env,"dalvik/system/DexFile");if(!type)return JNI_ERR;
 jint result=(*env)->UnregisterNatives(env,type);(*env)->DeleteLocalRef(env,type);return result;
}
