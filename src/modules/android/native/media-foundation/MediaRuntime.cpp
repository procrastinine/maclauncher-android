#include <media/NdkMediaFormat.h>
#include <jni.h>
// This library has no load-time JNI state. Export its own entry so a JVM
// lookup cannot invoke a dependency's JNI_OnLoad on behalf of this library.
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*,void*){return JNI_VERSION_1_6;}
#include "media-format.h"
namespace {
const KLJMediaFormatApi api={AMediaFormat_new,AMediaFormat_setInt32,AMediaFormat_setInt64,AMediaFormat_setFloat,AMediaFormat_setString,AMediaFormat_getString};
}
extern "C" bool klj_media_format_bind(void *library){return klj_media_format_install(&api,library);}
