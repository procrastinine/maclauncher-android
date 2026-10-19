#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "policy.h"

// This is the dedicated platform KeyStore's authorization backend. The kernel
// enforces the macOS process/file sandbox. Original keystore2 checks logical UID
// ownership, namespace access and grant vectors before calling this boundary.
static const char owner_context[]="u:r:keystore:s0";
struct selabel_handle {unsigned marker;};
int is_selinux_enabled(void){return 0;}
void freecon(char *context){free(context);}
static int copy_context(char **out,const char *value){
    if(!out){errno=EINVAL;return -1;}*out=strdup(value);return *out?0:-1;
}
int getcon(char **context){return copy_context(context,owner_context);}
int getprevcon(char **context){(void)context;errno=ENODATA;return -1;}
int getpidcon(pid_t pid,char **context){
    // No labels are inferred from another host process's numeric UID.
    if(pid!=getpid()){errno=EACCES;return -1;}return getcon(context);
}
int setcon(const char *context){
    // The supervisor owns this immutable process identity.
    if(context&&strcmp(context,owner_context)==0)return 0;
    errno=EACCES;return -1;
}
struct selabel_handle *selinux_android_keystore2_key_context_handle(void){
    struct selabel_handle *handle=malloc(sizeof(*handle));if(handle)handle->marker=SELABEL_CTX_ANDROID_KEYSTORE2_KEY;return handle;
}
struct selabel_handle *selinux_android_service_context_handle(void){
    struct selabel_handle *handle=malloc(sizeof(*handle));if(handle)handle->marker=SELABEL_CTX_ANDROID_SERVICE;return handle;
}
struct selabel_handle *selinux_android_vendor_service_context_handle(void){errno=ENOSYS;return NULL;}
// The policy is compiled and immutable, so its userspace snapshot never reloads.
int selinux_status_open(int fallback){(void)fallback;return 0;}
void selinux_status_close(void){}
int selinux_status_updated(void){return 0;}
void selabel_close(struct selabel_handle *handle){free(handle);}
int selabel_lookup(struct selabel_handle *handle,char **context,const char *key,int type){
    if(handle&&handle->marker==SELABEL_CTX_ANDROID_SERVICE&&type==SELABEL_CTX_ANDROID_SERVICE&&key&&context){
        const char *fallback=NULL;
        for(size_t i=0;i<sizeof(service_contexts)/sizeof(*service_contexts);++i){
            if(!strcmp(service_contexts[i].name,key))return copy_context(context,service_contexts[i].label);
            if(!strcmp(service_contexts[i].name,"*"))fallback=service_contexts[i].label;
        }
        if(fallback)return copy_context(context,fallback);errno=ENOENT;return -1;
    }
    if(!handle||handle->marker!=SELABEL_CTX_ANDROID_KEYSTORE2_KEY||type!=SELABEL_CTX_ANDROID_KEYSTORE2_KEY||!key||!*key||!context){errno=EINVAL;return -1;}
    char *end;errno=0;long id=strtol(key,&end,10);if(errno||*end||id<0){errno=EINVAL;return -1;}
    for(size_t i=0;i<sizeof(contexts)/sizeof(*contexts);++i)if(id==contexts[i].namespace_id)return copy_context(context,contexts[i].label);
    errno=ENOENT;return -1;
}
int selinux_check_access(const char *source,const char *target,const char *tclass,const char *permission,void *audit){
    (void)audit;
    if(!source||!target||!tclass||!permission){errno=EINVAL;return -1;}
    for(size_t i=0;i<sizeof(rules)/sizeof(*rules);++i){const struct Rule *r=&rules[i];
        if(!strcmp(source,r->source)&&!strcmp(target,r->target)&&!strcmp(tclass,r->tclass)&&!strcmp(permission,r->permission))return 0;
    }
    errno=EACCES;return -1;
}
// Original Rust logging initialization remains usable. This backend has no
// kernel AVC logger; denied operations return EACCES to the original caller.
void selinux_set_callback(int type,union selinux_callback callback){(void)type;(void)callback;}
int selinux_log_callback(int type,const char *format,...){
    (void)type;va_list values;va_start(values,format);int result=vfprintf(stderr,format,values);va_end(values);return result;
}
