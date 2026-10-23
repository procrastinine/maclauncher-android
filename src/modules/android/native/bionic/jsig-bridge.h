#ifndef KLJ_BIONIC_JSIG_BRIDGE_H
#define KLJ_BIONIC_JSIG_BRIDGE_H
#include <signal.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KLJ_JSIG_BRIDGE_ABI 1u
#define KLJ_JSIG_LIBRARY_ENV "KLJ_BIONIC_JSIG_LIBRARY"
typedef int(*KljJsigAction)(int,const struct sigaction*,struct sigaction*);
typedef struct{uint32_t size,version,outer_calls,real_calls,rejected_recursion;}KljJsigCounts;
typedef struct{
 uint32_t size,version;
 KljJsigAction(*original_action)(void);
 void(*counts)(KljJsigCounts*);
}KljJsigBridgeApi;
const KljJsigBridgeApi *klj_jsig_bridge_get_api(void);
#ifdef __cplusplus
}
#endif
#endif
