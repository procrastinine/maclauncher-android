#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include "JniConstants.h"

namespace {
jclass byteArrayClass;
}
jclass JniConstants::GetPrimitiveByteArrayClass(JNIEnv*) { return byteArrayClass; }
void register_libcore_io_Memory(JNIEnv*);
namespace art { void register_libcore_io_Memory(JNIEnv*); }

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm,void*) {
    JNIEnv* env=nullptr;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
    jclass local=env->FindClass("[B");if(!local)return JNI_ERR;
    byteArrayClass=static_cast<jclass>(env->NewGlobalRef(local));env->DeleteLocalRef(local);
    if(!byteArrayClass)return JNI_ERR;
    register_libcore_io_Memory(env);
    if(env->ExceptionCheck())return JNI_ERR;
    art::register_libcore_io_Memory(env);
    return env->ExceptionCheck()?JNI_ERR:JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostNio_freeDirectBuffer(JNIEnv* env,jclass,jobject buffer) {
    if(!buffer)return;
    jclass direct=env->FindClass("sun/nio/ch/DirectBuffer");if(!direct)return;
    if(!env->IsInstanceOf(buffer,direct)){
        env->DeleteLocalRef(direct);jniThrowException(env,"java/lang/ClassCastException","Buffer is not direct");return;
    }
    jmethodID cleaner=env->GetMethodID(direct,"cleaner","()Ljdk/internal/ref/Cleaner;");
    jmethodID attachment=env->GetMethodID(direct,"attachment","()Ljava/lang/Object;");
    if(!cleaner||!attachment){env->DeleteLocalRef(direct);return;}
    jobject owner=env->NewLocalRef(buffer),cleanup=nullptr;
    // JDK slices retain their actual allocation owner. No second free callback,
    // address registry or duplicate allocation is introduced here.
    while(owner&&!env->ExceptionCheck()){
        cleanup=env->CallObjectMethod(owner,cleaner);
        if(cleanup||env->ExceptionCheck())break;
        jobject next=env->CallObjectMethod(owner,attachment);
        env->DeleteLocalRef(owner);owner=nullptr;
        if(env->ExceptionCheck()) { if(next)env->DeleteLocalRef(next);break; }
        if(next&&env->IsInstanceOf(next,direct))owner=next;
        else if(next)env->DeleteLocalRef(next);
    }
    if(owner)env->DeleteLocalRef(owner);env->DeleteLocalRef(direct);
    if(cleanup&&!env->ExceptionCheck()){
        jclass type=env->GetObjectClass(cleanup);
        jmethodID clean=type?env->GetMethodID(type,"clean","()V"):nullptr;
        if(clean)env->CallVoidMethod(cleanup,clean);
        if(type)env->DeleteLocalRef(type);
    }
    if(cleanup)env->DeleteLocalRef(cleanup);
}

extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostNio_getFD(JNIEnv* env,jclass,jobject channel) {
    if(!channel){jniThrowNullPointerException(env,"channel");return nullptr;}
    jclass type=env->FindClass("sun/nio/ch/FileChannelImpl");if(!type)return nullptr;
    if(!env->IsInstanceOf(channel,type)){
        env->DeleteLocalRef(type);jniThrowException(env,"java/lang/ClassCastException","Channel is not a JVM file channel");return nullptr;
    }
    jfieldID field=env->GetFieldID(type,"fd","Ljava/io/FileDescriptor;");
    jobject result=field?env->GetObjectField(channel,field):nullptr;
    env->DeleteLocalRef(type);return result;
}

extern "C" JNIEXPORT jobject JNICALL Java_maclauncher_android_framework_HostNio_openChannel(JNIEnv* env,jclass,jobject parent,jobject descriptor,jboolean readable,jboolean writable) {
    jclass type=env->FindClass("sun/nio/ch/FileChannelImpl");if(!type)return nullptr;
    jmethodID open=env->GetStaticMethodID(type,"open","(Ljava/io/FileDescriptor;Ljava/lang/String;ZZZLjava/io/Closeable;)Ljava/nio/channels/FileChannel;");
    jobject result=open?env->CallStaticObjectMethod(type,open,descriptor,nullptr,readable,writable,JNI_FALSE,parent):nullptr;
    env->DeleteLocalRef(type);return result;
}
