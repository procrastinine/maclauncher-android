#include <jni.h>
#include <input/KeyCharacterMap.h>
#include <memory>

namespace android {
jobject android_view_KeyCharacterMap_create(JNIEnv*, int32_t,
        std::unique_ptr<KeyCharacterMap>);
}

extern "C" JNIEXPORT jobject JNICALL
Java_maclauncher_android_ArtPlatform_keyboard(JNIEnv* env, jclass, jstring text) {
    if (!text) {
        jclass type = env->FindClass("java/lang/NullPointerException");
        if (type) env->ThrowNew(type, "Keyboard map contents are required");
        return nullptr;
    }
    const char* contents = env->GetStringUTFChars(text, nullptr);
    if (!contents) return nullptr;
    auto result = android::KeyCharacterMap::loadContents("Virtual.kcm", contents,
            android::KeyCharacterMap::Format::BASE);
    env->ReleaseStringUTFChars(text, contents);
    if (!result.ok()) {
        jclass type = env->FindClass("java/lang/IllegalArgumentException");
        if (type) env->ThrowNew(type, result.error().message().c_str());
        return nullptr;
    }
    return android::android_view_KeyCharacterMap_create(env, -1,
            std::make_unique<android::KeyCharacterMap>(**result));
}
