#include "session-core.h"
#include <stddef.h>
#include <string.h>
enum {VIDEO=1,WINDOW=2,LAYER=4,DISPLAY=8};
static int owns(KLSession*s,uintptr_t owner){return s&&owner&&s->owner==owner;}
static int extent(int w,int h){return w>0&&h>0&&w<=8192&&h<=8192;}
int kl_session_init(KLSession*s,const KLSessionOps*ops,void*context,uintptr_t owner){
 if(!s||!ops||!context||!owner||!ops->video||!ops->window||!ops->layer||!ops->drawable||!ops->bind||!ops->claim||!ops->stop||!ops->unbind||!ops->destroy_layer||!ops->destroy_window||!ops->quit_video)return 0;
 *s=(KLSession){.ops=ops,.context=context,.owner=owner,.state=KL_SESSION_NEW};return 1;
}
int kl_session_open(KLSession*s,uintptr_t owner,int w,int h){
 if(!owns(s,owner)||s->state!=KL_SESSION_NEW||!extent(w,h))return 0;
 s->state=KL_SESSION_FAILED;
 if(!s->ops->video(s->context))return 0;s->acquired|=VIDEO;
 if(!s->ops->window(s->context,w,h))return 0;s->acquired|=WINDOW;
 if(!s->ops->layer(s->context))return 0;s->acquired|=LAYER;
 int committedW=0,committedH=0;
 /* Window requests are points; the drawable reports actual backing pixels. */
 if(!s->ops->drawable(s->context,w,h,&committedW,&committedH)||!extent(committedW,committedH))return 0;
 /* bind may publish and then throw; cleanup must still stop/unbind it. */
 s->acquired|=DISPLAY;
 if(!s->ops->bind(s->context,committedW,committedH))return 0;
 s->width=committedW;s->height=committedH;s->state=KL_SESSION_READY;return 1;
}
int kl_session_claim(KLSession*s,uintptr_t owner,KLSessionBranch branch,int w,int h){
 if(!owns(s,owner)||s->state!=KL_SESSION_READY||branch<KL_SESSION_GLES||branch>KL_SESSION_NATIVE||w!=s->width||h!=s->height)return 0;
 if(!s->ops->claim(s->context,branch))return 0;s->branch=branch;s->state=KL_SESSION_CLAIMED;return 1;
}
int kl_session_resize_ready(KLSession*s,uintptr_t owner,int w,int h){
 if(!owns(s,owner)||s->state!=KL_SESSION_READY||!extent(w,h))return 0;
 s->state=KL_SESSION_FAILED;int actualW=0,actualH=0;
 if(!s->ops->drawable(s->context,w,h,&actualW,&actualH)||!extent(actualW,actualH))return 0;
 if(!s->ops->bind(s->context,actualW,actualH))return 0;
 s->width=actualW;s->height=actualH;s->state=KL_SESSION_READY;return 1;
}
int kl_session_commit(KLSession*s,uintptr_t owner,KLSessionBranch branch,int w,int h){
 if(!owns(s,owner)||s->state!=KL_SESSION_CLAIMED||branch!=s->branch||!extent(w,h))return 0;
 if(w==s->width&&h==s->height)return 1;
 /* The caller supplies completed branch storage, never an SDL resize request. */
 if(!s->ops->bind(s->context,w,h))return 0;s->width=w;s->height=h;return 1;
}
int kl_session_stop(KLSession*s,uintptr_t owner){
 if(!owns(s,owner))return 0;if(!(s->acquired&DISPLAY))return 1;
 return s->ops->stop(s->context);
}
int kl_session_release(KLSession*s,uintptr_t owner,KLSessionBranch branch,int drained){
 if(!owns(s,owner)||s->state!=KL_SESSION_CLAIMED||branch!=s->branch||!drained)return 0;
 int stopped=kl_session_stop(s,owner);if(stopped!=1)return stopped;
 s->branch=0;s->state=KL_SESSION_RETIRED;return 1;
}
int kl_session_close(KLSession*s,uintptr_t owner){
 if(!owns(s,owner))return 0;if(s->state==KL_SESSION_CLOSED)return 1;
 if(s->state==KL_SESSION_CLAIMED)return KL_SESSION_BUSY;
 int stopped=kl_session_stop(s,owner);if(stopped!=1)return stopped;
 if(s->acquired&DISPLAY){if(!s->ops->unbind(s->context))return 0;s->acquired&=~DISPLAY;}
 if(s->acquired&LAYER){s->ops->destroy_layer(s->context);s->acquired&=~LAYER;}
 if(s->acquired&WINDOW){s->ops->destroy_window(s->context);s->acquired&=~WINDOW;}
 if(s->acquired&VIDEO){s->ops->quit_video(s->context);s->acquired&=~VIDEO;}
 s->state=KL_SESSION_CLOSED;return 1;
}
