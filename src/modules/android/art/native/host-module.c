#include "bridge.h"
#include "host-packets.h"
#include "native-clock/clock-domain.h"
#include "android-log.h"
#include <unistd.h>
extern int64_t maclauncher_art_frame(uint64_t,uint64_t);
extern int64_t maclauncher_art_window(uint64_t,uint64_t);
extern int64_t maclauncher_art_audio(uint32_t,uint64_t,uint64_t);
extern int64_t maclauncher_art_network(uint64_t,uint64_t);
extern int64_t maclauncher_art_media(uint64_t,uint64_t);
extern int64_t maclauncher_art_web_links(uint64_t,uint64_t);
#ifdef KLJ_ART_ARM32
extern int64_t maclauncher_art_arm32(uint64_t,uint64_t,KljArtCallback);
#endif
/* Fixed C packets cross the VM boundary. JNI references remain inside ART. */
__attribute__((visibility("default")))
int64_t maclauncher_art_host_call(uint32_t function,uint64_t address,uint64_t size,KljArtCallback callback){
 (void)callback;
 if(function>=0x10000&&function<=0x10005)return maclauncher_art_audio(function-0x10000,address,size);
 if(function==5)return maclauncher_art_frame(address,size);
 if(function==4)return maclauncher_art_window(address,size);
 if(function==6)return maclauncher_art_network(address,size);
 if(function==8)return maclauncher_art_media(address,size);
 if(function==10)return maclauncher_art_web_links(address,size);
 if(function==11){if(address||size)return -EINVAL;return getpid();}
 if(function==KLJ_ART_LOG_FUNCTION){
  if(!address&&!size){klj_android_log_initialize();return 0;}
  if(!address||size!=sizeof(struct KljArtLog))return -EINVAL;
  int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(struct KljArtLog),KLJ_ART_PACKET_READ,&address);if(packet_error)return packet_error;
  const struct KljArtLog *value=(const void *)(uintptr_t)address;
  return klj_android_log_write(value->priority,(const void *)(uintptr_t)value->tag,(const void *)(uintptr_t)value->message);
 }
#ifdef KLJ_ART_ARM32
 if(function==9)return maclauncher_art_arm32(address,size,callback);
#endif
 if(function==1){if(!address||size!=sizeof(uint64_t))return -EINVAL;int64_t packet_error=klj_art_resolve_packet(address,size,KLJ_ART_PACKET_ALIGN(uint64_t),KLJ_ART_PACKET_READ_WRITE,&address);if(packet_error)return packet_error;struct timespec value;if(klj_clock_read(7,&value))return -errno;*(uint64_t*)(uintptr_t)address=(uint64_t)value.tv_sec*1000000000ull+value.tv_nsec;return 0;}
 return -ENOSYS;
}
