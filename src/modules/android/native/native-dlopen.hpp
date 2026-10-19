#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
namespace native_dlopen {
inline void *(*guest_open)(const char*,int)=nullptr;
inline char *(*guest_error)()=nullptr;
inline thread_local char message[192];
inline thread_local bool pending=false;
inline char *error(){
 // Refusal drained older guest errors. Any newly pending guest/ANGLE error
 // therefore happened later and replaces this adapter's older message.
 if(auto latest=guest_error?guest_error():nullptr){pending=false;return latest;}
 if(pending){pending=false;return message;}return nullptr;
}
inline void *open(const char *name,int flags,const void *extension){
 uint64_t extra=0;if(extension)std::memcpy(&extra,extension,sizeof extra);
 if(extra){
  if(guest_error)guest_error();
  std::snprintf(message,sizeof message,"android_dlopen_ext options are unsupported: 0x%llx",(unsigned long long)extra);pending=true;return nullptr;
 }
 return guest_open?guest_open(name,flags):nullptr;
}
}
