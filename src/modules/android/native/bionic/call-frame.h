#pragma once
#include "call-packet.h"
/* Descriptor-derived Darwin JNI closure. Existing NativeABI parsing and signed
 * page publication are reused unchanged; legacy JNI closures remain separate. */
void *klj_bionic_native_abi(void *target,const char *signature,char *error,size_t capacity);
/* Required embedding boundary, called on the original Java physical thread.
 * Route the trusted target through its typed Bionic owner/admission token and
 * write the packet results. A rejected entry returns its negative status; the
 * embedding JNI owner must preserve/map its Java exception before returning.
 * There is no default executor, image cast, admission or activation here. */
int64_t klj_bionic_call_frame_invoke(KljCallPacket *packet);
