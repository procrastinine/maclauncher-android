#ifndef MACLAUNCHER_ANGLE_DEBUG_CALLBACK_H
#define MACLAUNCHER_ANGLE_DEBUG_CALLBACK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*KljAngleDebugCallback)(uint32_t,uint32_t,uint32_t,uint32_t,int32_t,const char*,const void*);
typedef void (*KljAngleDebugInstall)(KljAngleDebugCallback,const void*);
typedef struct {
    uint32_t size,version;
    void *payload;
    void (*release)(void*);
} KljAngleDebugOwnerV1;
typedef int (*KljAngleDebugOwnerInstall)(KljAngleDebugInstall,KljAngleDebugCallback,const void*,const KljAngleDebugOwnerV1*);
#ifdef __cplusplus
}
#endif

#endif
