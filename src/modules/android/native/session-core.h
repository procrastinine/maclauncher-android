#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {KL_SESSION_NEW,KL_SESSION_READY,KL_SESSION_CLAIMED,KL_SESSION_RETIRED,KL_SESSION_FAILED,KL_SESSION_CLOSED} KLSessionState;
typedef enum {KL_SESSION_GLES=1,KL_SESSION_NATIVE=2} KLSessionBranch;
typedef enum {KL_SESSION_OK=1,KL_SESSION_ERROR=0,KL_SESSION_BUSY=-1} KLSessionResult;
typedef struct {
 int (*video)(void*),(*window)(void*,int,int),(*layer)(void*),(*drawable)(void*,int,int,int*,int*),(*bind)(void*,int,int),(*claim)(void*,KLSessionBranch);
 int (*stop)(void*); /* 1 retired, -1 in flight, 0 failure; no resource release */
 int (*unbind)(void*);
 void (*destroy_layer)(void*),(*destroy_window)(void*),(*quit_video)(void*);
} KLSessionOps;
typedef struct {
 const KLSessionOps*ops;void*context;uintptr_t owner;KLSessionState state;KLSessionBranch branch;
 unsigned acquired;int width,height; /* committed output pixels; adapter maps the virtual source extent */
} KLSession;
int kl_session_init(KLSession*,const KLSessionOps*,void*,uintptr_t owner);
/* Initial window and resize requests are logical points; drawable returns pixels. */
int kl_session_open(KLSession*,uintptr_t owner,int width,int height);
/* Before a producer adopts the window: verify one new drawable, then publish pixels. */
int kl_session_resize_ready(KLSession*,uintptr_t owner,int width,int height);
int kl_session_claim(KLSession*,uintptr_t owner,KLSessionBranch,int width,int height);
int kl_session_commit(KLSession*,uintptr_t owner,KLSessionBranch,int width,int height);
int kl_session_stop(KLSession*,uintptr_t owner);
int kl_session_release(KLSession*,uintptr_t owner,KLSessionBranch,int producerDrained);
int kl_session_close(KLSession*,uintptr_t owner);
#ifdef __cplusplus
}
#endif
