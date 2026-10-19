/*
 * Copyright 2019, The Android Open Source Project
 * Copyright 2026, MacLauncher contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// ConvertKeyValueArraysToMessage from frameworks/base/media/jni/android_media_Streams.cpp,
// revision 4e43ad12e1b211f15152e9c5b16b0fe88e6b93f3. All original value types and
// private-key filtering remain. JNI checks, scoped references, and ByteBuffer
// slice/read-only handling adapt the boundary to the shared native media owner.
#include <jni.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <nativehelper/scoped_local_ref.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

void throwConversionException(JNIEnv *env, const char *type, const char *message) {
    if (env->ExceptionCheck()) return;
    ScopedLocalRef<jclass> exceptionClass(env, env->FindClass(type));
    if (exceptionClass.get() != nullptr) env->ThrowNew(exceptionClass.get(), message);
}

// The input is checked for null before construction. Keeping the acquired UTF
// chars scoped also releases them if an original native allocation throws.
class UtfChars {
public:
    UtfChars(JNIEnv *env, jstring value)
        : mEnv(env), mValue(value), mChars(env->GetStringUTFChars(value, nullptr)) {}
    ~UtfChars() { if (mChars != nullptr) mEnv->ReleaseStringUTFChars(mValue, mChars); }
    const char *get() const { return mChars; }
private:
    JNIEnv *mEnv;
    jstring mValue;
    const char *mChars;
    UtfChars(const UtfChars &) = delete;
    UtfChars &operator=(const UtfChars &) = delete;
};

android::status_t copyByteBuffer(JNIEnv *env, jobject valueObj, jclass byteBufClass,
                                android::sp<android::ABuffer> *out) {
    jmethodID positionID = env->GetMethodID(byteBufClass, "position", "()I");
    if (positionID == nullptr || env->ExceptionCheck()) return -EINVAL;
    jmethodID limitID = env->GetMethodID(byteBufClass, "limit", "()I");
    if (limitID == nullptr || env->ExceptionCheck()) return -EINVAL;
    jmethodID capacityID = env->GetMethodID(byteBufClass, "capacity", "()I");
    if (capacityID == nullptr || env->ExceptionCheck()) return -EINVAL;

    jint position = env->CallIntMethod(valueObj, positionID);
    if (env->ExceptionCheck()) return -EINVAL;
    jint limit = env->CallIntMethod(valueObj, limitID);
    if (env->ExceptionCheck()) return -EINVAL;
    jint capacity = env->CallIntMethod(valueObj, capacityID);
    if (env->ExceptionCheck()) return -EINVAL;
    if (position < 0 || limit < position || capacity < limit) return -EINVAL;

    const jsize size = limit - position;
    android::sp<android::ABuffer> buffer = new android::ABuffer(size);
    if (size != 0 && buffer->data() == nullptr) return -ENOMEM;
    if (size == 0) {
        *out = buffer;
        return android::OK;
    }

    void *data = env->GetDirectBufferAddress(valueObj);
    if (env->ExceptionCheck()) return -EINVAL;
    jlong directCapacity = env->GetDirectBufferCapacity(valueObj);
    if (env->ExceptionCheck()) return -EINVAL;
    if (directCapacity >= 0) {
        if (data == nullptr || directCapacity < limit) return -EINVAL;
        memcpy(buffer->data(), static_cast<const uint8_t *>(data) + position, size);
    } else {
        jmethodID hasArrayID = env->GetMethodID(byteBufClass, "hasArray", "()Z");
        if (hasArrayID == nullptr || env->ExceptionCheck()) return -EINVAL;
        jboolean hasArray = env->CallBooleanMethod(valueObj, hasArrayID);
        if (env->ExceptionCheck()) return -EINVAL;

        if (hasArray) {
            jmethodID arrayID = env->GetMethodID(byteBufClass, "array", "()[B");
            if (arrayID == nullptr || env->ExceptionCheck()) return -EINVAL;
            jmethodID arrayOffsetID = env->GetMethodID(byteBufClass, "arrayOffset", "()I");
            if (arrayOffsetID == nullptr || env->ExceptionCheck()) return -EINVAL;
            ScopedLocalRef<jbyteArray> byteArray(env,
                static_cast<jbyteArray>(env->CallObjectMethod(valueObj, arrayID)));
            if (env->ExceptionCheck() || byteArray.get() == nullptr) return -EINVAL;
            jint arrayOffset = env->CallIntMethod(valueObj, arrayOffsetID);
            if (env->ExceptionCheck()) return -EINVAL;
            jsize arraySize = env->GetArrayLength(byteArray.get());
            if (env->ExceptionCheck()) return -EINVAL;
            if (arrayOffset < 0 || static_cast<jlong>(arrayOffset) + limit > arraySize)
                return -EINVAL;
            env->GetByteArrayRegion(byteArray.get(), arrayOffset + position, size,
                                    reinterpret_cast<jbyte *>(buffer->data()));
            if (env->ExceptionCheck()) return -EINVAL;
        } else {
            // A read-only heap buffer has no accessible array. Reading a
            // duplicate preserves the supplied buffer's position and mark.
            jmethodID duplicateID = env->GetMethodID(byteBufClass, "duplicate",
                                                     "()Ljava/nio/ByteBuffer;");
            if (duplicateID == nullptr || env->ExceptionCheck()) return -EINVAL;
            jmethodID getID = env->GetMethodID(byteBufClass, "get",
                                               "([B)Ljava/nio/ByteBuffer;");
            if (getID == nullptr || env->ExceptionCheck()) return -EINVAL;
            ScopedLocalRef<jobject> duplicate(env, env->CallObjectMethod(valueObj, duplicateID));
            if (env->ExceptionCheck() || duplicate.get() == nullptr) return -EINVAL;
            ScopedLocalRef<jbyteArray> byteArray(env, env->NewByteArray(size));
            if (env->ExceptionCheck() || byteArray.get() == nullptr) return -ENOMEM;
            ScopedLocalRef<jobject> result(env,
                env->CallObjectMethod(duplicate.get(), getID, byteArray.get()));
            if (env->ExceptionCheck() || result.get() == nullptr) return -EINVAL;
            env->GetByteArrayRegion(byteArray.get(), 0, size,
                                    reinterpret_cast<jbyte *>(buffer->data()));
            if (env->ExceptionCheck()) return -EINVAL;
        }
    }

    *out = buffer;
    return android::OK;
}

}  // namespace

namespace android {

status_t ConvertKeyValueArraysToMessage(
        JNIEnv *env, jobjectArray keys, jobjectArray values,
        sp<AMessage> *out) {
    if (env->ExceptionCheck()) return -EINVAL;
    ScopedLocalRef<jclass> stringClass(env, env->FindClass("java/lang/String"));
    if (stringClass.get() == nullptr || env->ExceptionCheck()) return -EINVAL;
    ScopedLocalRef<jclass> integerClass(env, env->FindClass("java/lang/Integer"));
    if (integerClass.get() == nullptr || env->ExceptionCheck()) return -EINVAL;
    ScopedLocalRef<jclass> longClass(env, env->FindClass("java/lang/Long"));
    if (longClass.get() == nullptr || env->ExceptionCheck()) return -EINVAL;
    ScopedLocalRef<jclass> floatClass(env, env->FindClass("java/lang/Float"));
    if (floatClass.get() == nullptr || env->ExceptionCheck()) return -EINVAL;
    ScopedLocalRef<jclass> byteBufClass(env, env->FindClass("java/nio/ByteBuffer"));
    if (byteBufClass.get() == nullptr || env->ExceptionCheck()) return -EINVAL;

    sp<AMessage> msg = new AMessage;
    jsize numEntries = 0;

    if (keys != nullptr) {
        if (values == nullptr) return -EINVAL;
        numEntries = env->GetArrayLength(keys);
        if (env->ExceptionCheck()) return -EINVAL;
        jsize numValues = env->GetArrayLength(values);
        if (env->ExceptionCheck() || numEntries != numValues) return -EINVAL;
    } else if (values != nullptr) {
        return -EINVAL;
    }

    for (jsize i = 0; i < numEntries; ++i) {
        ScopedLocalRef<jobject> keyObj(env, env->GetObjectArrayElement(keys, i));
        if (env->ExceptionCheck()) return -EINVAL;
        if (keyObj.get() == nullptr || !env->IsInstanceOf(keyObj.get(), stringClass.get()))
            return -EINVAL;

        UtfChars tmp(env, static_cast<jstring>(keyObj.get()));
        if (tmp.get() == nullptr || env->ExceptionCheck()) return -ENOMEM;
        AString key = tmp.get();

        if (key.startsWith("android._")) {
            // Don't propagate private keys, as in the original conversion.
            continue;
        }
        if (!msg->contains(key.c_str()) && msg->countEntries() >= AMessage::maxAllowedEntries())
            return -EINVAL;

        ScopedLocalRef<jobject> valueObj(env, env->GetObjectArrayElement(values, i));
        if (env->ExceptionCheck() || valueObj.get() == nullptr) return -EINVAL;

        if (env->IsInstanceOf(valueObj.get(), stringClass.get())) {
            UtfChars value(env, static_cast<jstring>(valueObj.get()));
            if (value.get() == nullptr || env->ExceptionCheck()) return -ENOMEM;
            msg->setString(key.c_str(), value.get());
        } else if (env->IsInstanceOf(valueObj.get(), integerClass.get())) {
            jmethodID intValueID = env->GetMethodID(integerClass.get(), "intValue", "()I");
            if (intValueID == nullptr || env->ExceptionCheck()) return -EINVAL;
            jint value = env->CallIntMethod(valueObj.get(), intValueID);
            if (env->ExceptionCheck()) return -EINVAL;
            msg->setInt32(key.c_str(), value);
        } else if (env->IsInstanceOf(valueObj.get(), longClass.get())) {
            jmethodID longValueID = env->GetMethodID(longClass.get(), "longValue", "()J");
            if (longValueID == nullptr || env->ExceptionCheck()) return -EINVAL;
            jlong value = env->CallLongMethod(valueObj.get(), longValueID);
            if (env->ExceptionCheck()) return -EINVAL;
            msg->setInt64(key.c_str(), value);
        } else if (env->IsInstanceOf(valueObj.get(), floatClass.get())) {
            jmethodID floatValueID = env->GetMethodID(floatClass.get(), "floatValue", "()F");
            if (floatValueID == nullptr || env->ExceptionCheck()) return -EINVAL;
            jfloat value = env->CallFloatMethod(valueObj.get(), floatValueID);
            if (env->ExceptionCheck()) return -EINVAL;
            msg->setFloat(key.c_str(), value);
        } else if (env->IsInstanceOf(valueObj.get(), byteBufClass.get())) {
            sp<ABuffer> buffer;
            status_t status = copyByteBuffer(env, valueObj.get(), byteBufClass.get(), &buffer);
            if (status != OK) return status;
            msg->setBuffer(key.c_str(), buffer);
        } else {
            // Unsupported values must not silently disappear from a format.
            return -EINVAL;
        }
    }

    *out = msg;
    return OK;
}

}  // namespace android

extern "C" AMediaFormat *klj_media_format_arrays(
        JNIEnv *env, jobjectArray keys, jobjectArray values) {
    if (env == nullptr || env->ExceptionCheck()) return nullptr;
    try {
        android::sp<android::AMessage> message;
        android::status_t status = android::ConvertKeyValueArraysToMessage(env, keys, values, &message);
        if (status != android::OK || env->ExceptionCheck()) {
            if (status == -ENOMEM) {
                throwConversionException(env, "java/lang/OutOfMemoryError", "Media format allocation failed");
            } else {
                throwConversionException(env, "java/lang/IllegalArgumentException", "Invalid media format values");
            }
            return nullptr;
        }
        AMediaFormat *format = android::AMediaFormat_fromMsg(&message);
        if (format == nullptr) {
            throwConversionException(env, "java/lang/OutOfMemoryError", "Media format allocation failed");
        }
        return format;
    } catch (const std::bad_alloc &) {
        throwConversionException(env, "java/lang/OutOfMemoryError", "Media format allocation failed");
        return nullptr;
    }
}
