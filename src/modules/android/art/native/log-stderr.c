#include <android/log.h>
#include "bridge.h"
static void output(const struct __android_log_message *message){
 struct KljArtLog packet={.priority=message->priority,.tag=(uintptr_t)message->tag,.message=(uintptr_t)message->message};
 (void)klj_art_host(KLJ_ART_LOG_FUNCTION,&packet,sizeof packet);
}
void klj_art_log_initialize(void) {
 (void)klj_art_host(KLJ_ART_LOG_FUNCTION,0,0);
 // Output transport does not change Android's process or per-tag log policy.
 __android_log_set_logger(output);
}
__attribute__((constructor)) static void start(void) { klj_art_log_initialize(); }
