#pragma once

// API 36's ByteBuffer overloads retain their upstream Java validation and
// position handling. These native boundaries operate on the same AOSP Parcel.
static android::Parcel* kljMarshallParcel(JNIEnv* env,jlong pointer,jint length){
    auto* parcel=reinterpret_cast<android::Parcel*>(pointer);
    if(!parcel)return nullptr;
    if(parcel->isForRpc()||parcel->objectsCount()){
        jniThrowException(env,"java/lang/RuntimeException","Parcel with Binder or file descriptor objects cannot be marshalled");return nullptr;
    }
    if(length<0||parcel->dataSize()>static_cast<size_t>(length)){
        jniThrowException(env,"java/nio/BufferOverflowException",nullptr);return nullptr;
    }
    return parcel;
}
static bool kljBufferRange(JNIEnv* env,jlong capacity,jint offset,jint length){
    if(offset>=0&&length>=0&&capacity>=0&&static_cast<jlong>(offset)+length<=capacity)return true;
    jniThrowException(env,"java/lang/IndexOutOfBoundsException","Parcel buffer range is invalid");return false;
}
static jint kljParcelMarshallArray(JNIEnv* env,jclass,jlong pointer,jbyteArray bytes,jint offset,jint length){
    if(!bytes){jniThrowNullPointerException(env,nullptr);return 0;}
    if(!kljBufferRange(env,env->GetArrayLength(bytes),offset,length))return 0;
    auto* parcel=kljMarshallParcel(env,pointer,length);if(!parcel)return 0;
    env->SetByteArrayRegion(bytes,offset,parcel->dataSize(),reinterpret_cast<const jbyte*>(parcel->data()));
    return env->ExceptionCheck()?0:parcel->dataSize();
}
static jint kljParcelMarshallBuffer(JNIEnv* env,jclass,jlong pointer,jobject bytes,jint offset,jint length){
    if(!bytes){jniThrowNullPointerException(env,nullptr);return 0;}
    if(!kljBufferRange(env,env->GetDirectBufferCapacity(bytes),offset,length))return 0;
    auto* parcel=kljMarshallParcel(env,pointer,length);if(!parcel)return 0;
    void* data=env->GetDirectBufferAddress(bytes);
    if(parcel->dataSize())memcpy(static_cast<uint8_t*>(data)+offset,parcel->data(),parcel->dataSize());
    return parcel->dataSize();
}
static void kljParcelUnmarshallBuffer(JNIEnv* env,jclass,jlong pointer,jobject bytes,jint offset,jint length){
    if(!bytes){jniThrowNullPointerException(env,nullptr);return;}
    if(!kljBufferRange(env,env->GetDirectBufferCapacity(bytes),offset,length))return;
    auto* parcel=reinterpret_cast<android::Parcel*>(pointer);if(!parcel)return;
    auto* data=static_cast<uint8_t*>(env->GetDirectBufferAddress(bytes));
    android::status_t status=parcel->setData(data?data+offset:nullptr,length);
    if(status!=android::OK)android::signalExceptionForError(env,nullptr,status);
}
