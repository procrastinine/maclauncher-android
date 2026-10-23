#ifndef MACLAUNCHER_ANGLE_BLOB_CALLBACK_H
#define MACLAUNCHER_ANGLE_BLOB_CALLBACK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*KljAngleSetBlob)(const void*,intptr_t,const void*,intptr_t,const void*);
typedef intptr_t (*KljAngleGetBlob)(const void*,intptr_t,void*,intptr_t,const void*);
typedef void (*KljAngleBlobInstall)(KljAngleSetBlob,KljAngleGetBlob,const void*);
typedef struct {uint32_t size,version;void *payload;void(*release)(void*);} KljAngleBlobOwnerV1;
typedef int (*KljAngleBlobOwnerInstall)(KljAngleBlobInstall,KljAngleSetBlob,KljAngleGetBlob,const void*,const KljAngleBlobOwnerV1*);
#ifdef __cplusplus
}
#endif
#endif
