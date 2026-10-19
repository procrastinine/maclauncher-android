#include <jni.h>
#include "webkit-physical-binding.h"
extern "C" void* klj_session_native_content(JNIEnv*);

// HotSpot supplies only its existing window capability. Browser operations and
// JNI method/callback marshaling are shared with the ART backend.
namespace klj::web {
Value request(void*thread,Op operation,const Args&arguments,std::shared_ptr<Callback>callback){
 static auto context=klj_webkit_context();
 return physicalRequest(context,operation,arguments,std::move(callback),klj_session_native_content(static_cast<JNIEnv*>(thread)));
}
}
