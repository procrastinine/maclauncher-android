#include "hotspot-jni.h"
#include "core_jni_helpers.h"
#include "android_os_Parcel.h"
#include "android_util_Binder.h"
#include <binder/Binder.h>
#include <binder/Parcel.h>

namespace {
jclass handles;
jmethodID retainHandle,stateForHandle;
void (*releaseHandle)(void*);
class ObjectBinder final : public android::BBinder {
public:
    const jlong handle;
    explicit ObjectBinder(jlong value):handle(value){}
    ~ObjectBinder() override{releaseHandle(reinterpret_cast<void*>(handle));}
};
}

int kljRegisterObjectParcel(JNIEnv* env){
    jclass owner=env->FindClass("maclauncher/android/framework/HostNativeHandle");if(!owner)return -1;
    handles=static_cast<jclass>(env->NewGlobalRef(owner));env->DeleteLocalRef(owner);if(!handles)return -1;
    retainHandle=env->GetStaticMethodID(handles,"retain","(J)J");if(!retainHandle)return -1;
    stateForHandle=env->GetStaticMethodID(handles,"state","(J)Ljava/lang/Object;");if(!stateForHandle)return -1;
    jmethodID finalizer=env->GetStaticMethodID(handles,"finalizer","()J");if(!finalizer)return -1;
    releaseHandle=reinterpret_cast<void(*)(void*)>(env->CallStaticLongMethod(handles,finalizer));
    return env->ExceptionCheck()||!releaseHandle?-1:0;
}
void kljReleaseObjectParcelMetadata(JNIEnv* env){if(handles){env->DeleteGlobalRef(handles);handles=nullptr;}}

extern "C" JNIEXPORT void JNICALL Java_maclauncher_android_framework_HostObjectParcel_writeNativeHandle(JNIEnv* env,jclass,jobject object,jlong handle){
    android::Parcel* parcel=android::parcelForJavaObject(env,object);if(!parcel)return;
    android::sp<android::IBinder> token;
    if(handle){
        jlong retained=env->CallStaticLongMethod(handles,retainHandle,handle);if(env->ExceptionCheck())return;
        try{token=android::sp<ObjectBinder>::make(retained);}
        catch(...){releaseHandle(reinterpret_cast<void*>(retained));jniThrowException(env,"java/lang/OutOfMemoryError","Parcel object allocation failed");return;}
    }
    android::status_t status=parcel->writeStrongBinder(token);
    if(status!=android::OK)android::signalExceptionForError(env,nullptr,status);
}
extern "C" JNIEXPORT jlong JNICALL Java_maclauncher_android_framework_HostObjectParcel_readNativeHandle(JNIEnv* env,jclass,jobject object,jclass expected){
    android::Parcel* parcel=android::parcelForJavaObject(env,object);if(!parcel)return 0;
    android::sp<android::IBinder> binder;android::status_t status=parcel->readNullableStrongBinder(&binder);
    if(status!=android::OK){android::signalExceptionForError(env,nullptr,status);return 0;}
    if(!binder)return 0;
    auto* token=dynamic_cast<ObjectBinder*>(binder.get());
    if(!token){jniThrowException(env,"android/os/BadParcelableException","Parcel native object has an incompatible owner");return 0;}
    ScopedLocalRef<jobject> state(env,env->CallStaticObjectMethod(handles,stateForHandle,token->handle));
    if(env->ExceptionCheck())return 0;
    if(!env->IsInstanceOf(state.get(),expected)){jniThrowException(env,"android/os/BadParcelableException","Parcel native object has an incompatible type");return 0;}
    return env->CallStaticLongMethod(handles,retainHandle,token->handle);
}
