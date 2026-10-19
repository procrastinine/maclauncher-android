#include <jni.h>
#include <jvmti.h>
#include <string.h>
#include <stdio.h>

static void fail(JNIEnv *env,const char *message){
    if((*env)->ExceptionCheck(env))return;
    jclass type=(*env)->FindClass(env,"java/lang/IllegalStateException");
    if(type){(*env)->ThrowNew(env,type,message);(*env)->DeleteLocalRef(env,type);}
}
static void release(jvmtiEnv *ti,void *memory){if(memory)(*ti)->Deallocate(ti,memory);}
static void finish(jvmtiEnv *ti,void *methods){release(ti,methods);(*ti)->DisposeEnvironment(ti);}
JNIEXPORT jobjectArray JNICALL Java_maclauncher_android_bridge_NativeBinding_nativeMethods(JNIEnv *env,jclass ignored,jclass target){
    if((*env)->ExceptionCheck(env))return NULL;
    if(!target){fail(env,"Native method class is missing");return NULL;}
    JavaVM *vm=NULL;jvmtiEnv *ti=NULL;
    if((*env)->GetJavaVM(env,&vm)!=JNI_OK||(*vm)->GetEnv(vm,(void**)&ti,JVMTI_VERSION_1_2)!=JNI_OK){fail(env,"Native method metadata is unavailable");return NULL;}
    jmethodID *methods=NULL;jint count=0,natives=0;
    jvmtiError error=(*ti)->GetClassMethods(ti,target,&count,&methods);
    if(error!=JVMTI_ERROR_NONE){
        finish(ti,methods);
        // Null without an exception means only CLASS_NOT_PREPARED. Java links
        // the exact class without initializing it, then makes one retry.
        if(error!=JVMTI_ERROR_CLASS_NOT_PREPARED)fail(env,"Native method metadata could not be read");return NULL;
    }
    if(count<0||count>65535){finish(ti,methods);fail(env,"Native method class exceeds supported bounds");return NULL;}
    for(jint i=0;i<count;i++){
        jint modifiers=0;error=(*ti)->GetMethodModifiers(ti,methods[i],&modifiers);
        if(error!=JVMTI_ERROR_NONE){finish(ti,methods);fail(env,"Native method modifiers could not be read");return NULL;}
        if(modifiers&0x100)natives++;
    }
    jclass string=(*env)->FindClass(env,"java/lang/String");
    if(!string){finish(ti,methods);return NULL;}
    jobjectArray result=(*env)->NewObjectArray(env,natives*3,string,NULL);(*env)->DeleteLocalRef(env,string);
    if(!result){finish(ti,methods);return NULL;}
    jint output=0;
    for(jint i=0;i<count;i++){
        jint modifiers=0;error=(*ti)->GetMethodModifiers(ti,methods[i],&modifiers);
        if(error!=JVMTI_ERROR_NONE){fail(env,"Native method modifiers could not be read");goto failure;}
        if(!(modifiers&0x100))continue;
        char *name=NULL,*signature=NULL;
        error=(*ti)->GetMethodName(ti,methods[i],&name,&signature,NULL);
        if(error!=JVMTI_ERROR_NONE||!name||!signature){
            release(ti,name);release(ti,signature);fail(env,"Native method descriptor could not be read");goto failure;
        }
        if((*env)->PushLocalFrame(env,4)<0){release(ti,name);release(ti,signature);goto failure;}
        jstring values[3]={NULL,NULL,NULL};
        values[0]=(*env)->NewStringUTF(env,name);
        if(values[0])values[1]=(*env)->NewStringUTF(env,signature);
        if(values[1])values[2]=(*env)->NewStringUTF(env,(modifiers&0x8)?"static":"instance");
        release(ti,name);release(ti,signature);
        if(values[2])for(jint n=0;n<3;n++){(*env)->SetObjectArrayElement(env,result,output+n,values[n]);if((*env)->ExceptionCheck(env))break;}
        jboolean failed=(*env)->ExceptionCheck(env)||!values[2];
        (*env)->PopLocalFrame(env,NULL);
        if(failed)goto failure;
        output+=3;
    }
    finish(ti,methods);return result;
failure:
    finish(ti,methods);(*env)->DeleteLocalRef(env,result);return NULL;
}
