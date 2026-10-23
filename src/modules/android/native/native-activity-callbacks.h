#pragma once
#include "native-image-provider.h"

/* A NativeActivity callback contains only GP arguments, at most three, in the
 * original NDK header. This constructs the already shared AAPCS64 packet; it
 * does not parse a descriptor, classify a new ABI, or invoke legacy pointers.
 * Caller-owned data/rect/out-size storage remains live until return.
 * A zero return requests the existing KLJ_INVOKE_GUEST statement unchanged.
 * A one return includes a provider failure; no direct call is then permitted.
 * This helper alone does not supply NativeActivity resource/JNI ownership. */
static inline int klj_native_activity_callback(JNIEnv *env,jlong image,
        const void *function,uint64_t first,uint64_t second,uint64_t third,
        uint64_t *returned){
    KljCallPacket packet={.size=sizeof packet,.version=KLJ_CALL_PACKET_VERSION,
        .target=(uintptr_t)function,.x={first,second,third}};
    int owned=klj_native_image_invoke(env,image,&packet);
    if(owned&&!packet.status&&returned)*returned=packet.result_x[0];
    return owned;
}
