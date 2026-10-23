#ifndef MACLAUNCHER_CHOREOGRAPHER_CALLBACK_OWNER_H
#define MACLAUNCHER_CHOREOGRAPHER_CALLBACK_OWNER_H
#include <stdint.h>
/* The native display owner copies this execution-domain descriptor into its
 * queued frame or refresh registration. No independent callback allocation. */
typedef struct { uintptr_t function,user,caller; } KljChoreographerCallback;
enum { KLJ_CHOREOGRAPHER_FRAME_LONG=1, KLJ_CHOREOGRAPHER_FRAME64=2,
    KLJ_CHOREOGRAPHER_VSYNC=3, KLJ_CHOREOGRAPHER_REFRESH=4,
    KLJ_CHOREOGRAPHER_UNREGISTER_REFRESH=5 };
/* Success is 1; rejected scheduling is 0. Public void APIs retain their
 * original return contract. Refresh identity is function+user, not caller. */
typedef int (*KljChoreographerCallbackInstall)(void *choreographer,unsigned kind,
    void *nativeCallback,const KljChoreographerCallback *callback,int64_t delayMillis);
#endif
