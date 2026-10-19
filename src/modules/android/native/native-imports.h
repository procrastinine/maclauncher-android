#ifndef MACLAUNCHER_NATIVE_IMPORTS_H
#define MACLAUNCHER_NATIVE_IMPORTS_H
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Bionic barrier storage is only four-byte aligned and cannot hold a Darwin
// mutex/condition. The bounded registry owns native state by guest address;
// wait pins it until return, and destroy never frees an active waiter.
// Barriers synchronize threads in this process. Shared-process attributes are
// explicitly refused, as Darwin condition variables cannot provide that ABI.
enum { KLJ_BARRIER_BUCKETS=1024, KLJ_BARRIER_LIMIT=65536 };
typedef struct klj_barrier {
    void *guest;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    unsigned count,arrived,users;
    uint64_t generation;
    int broken;
    struct klj_barrier *next;
} klj_barrier;
static pthread_mutex_t klj_barrier_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static klj_barrier *klj_barriers[KLJ_BARRIER_BUCKETS];
static unsigned klj_barrier_count;
static klj_barrier **klj_barrier_entry(void *guest) {
    uintptr_t key=(uintptr_t)guest;
    klj_barrier **entry=&klj_barriers[((key>>2)^(key>>12))&(KLJ_BARRIER_BUCKETS-1)];
    while(*entry&&(*entry)->guest!=guest)entry=&(*entry)->next;
    return entry;
}
static int klj_barrier_init(void *guest,const void *attribute,unsigned count) {
    if(!guest||!count)return kl_errno_to_linux(EINVAL);
    unsigned flags=0;if(attribute)memcpy(&flags,attribute,sizeof(flags));
    if(flags&1)return kl_errno_to_linux(ENOTSUP);
    klj_barrier *fresh=calloc(1,sizeof(*fresh));
    if(!fresh)return kl_errno_to_linux(ENOMEM);
    int error=pthread_mutex_init(&fresh->mutex,NULL);
    if(error){free(fresh);return kl_errno_to_linux(error);}
    error=pthread_cond_init(&fresh->changed,NULL);
    if(error){pthread_mutex_destroy(&fresh->mutex);free(fresh);return kl_errno_to_linux(error);}
    fresh->guest=guest;fresh->count=count;
    pthread_mutex_lock(&klj_barrier_registry_lock);
    klj_barrier **entry=klj_barrier_entry(guest);
    if(*entry)error=EBUSY;
    else if(klj_barrier_count==KLJ_BARRIER_LIMIT)error=EAGAIN;
    else {*entry=fresh;++klj_barrier_count;}
    pthread_mutex_unlock(&klj_barrier_registry_lock);
    if(error){pthread_cond_destroy(&fresh->changed);pthread_mutex_destroy(&fresh->mutex);free(fresh);}
    return kl_errno_to_linux(error);
}
static int klj_barrier_destroy(void *guest) {
    if(!guest)return kl_errno_to_linux(EINVAL);
    pthread_mutex_lock(&klj_barrier_registry_lock);
    klj_barrier **entry=klj_barrier_entry(guest),*state=*entry;
    int error=!state?EINVAL:state->users?EBUSY:0;
    if(!error){*entry=state->next;--klj_barrier_count;}
    pthread_mutex_unlock(&klj_barrier_registry_lock);
    if(!error){pthread_cond_destroy(&state->changed);pthread_mutex_destroy(&state->mutex);free(state);}
    return kl_errno_to_linux(error);
}
static int klj_barrier_wait(void *guest) {
    if(!guest)return kl_errno_to_linux(EINVAL);
    // pthread_barrier_wait is not a cancellation point. Darwin's condition
    // wait is, so defer cancellation until all state/ownership is released.
    int previous;int error=pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous);
    if(error)return kl_errno_to_linux(error);
    pthread_mutex_lock(&klj_barrier_registry_lock);
    klj_barrier *state=*klj_barrier_entry(guest);
    error=!state?EINVAL:state->users==UINT_MAX?EAGAIN:0;
    if(!error)++state->users;
    pthread_mutex_unlock(&klj_barrier_registry_lock);
    if(error){pthread_setcancelstate(previous,NULL);return kl_errno_to_linux(error);}
    pthread_mutex_lock(&state->mutex);
    int result=state->broken;
    if(!result){
        const uint64_t generation=state->generation;
        if(++state->arrived==state->count){
            state->arrived=0;++state->generation;
            pthread_cond_broadcast(&state->changed);
            result=-1; // Bionic PTHREAD_BARRIER_SERIAL_THREAD.
        }else {
            while(generation==state->generation&&!state->broken){
                error=pthread_cond_wait(&state->changed,&state->mutex);
                if(error){state->broken=kl_errno_to_linux(error);pthread_cond_broadcast(&state->changed);}
            }
            result=state->broken;
        }
    }
    pthread_mutex_unlock(&state->mutex);
    pthread_mutex_lock(&klj_barrier_registry_lock);--state->users;pthread_mutex_unlock(&klj_barrier_registry_lock);
    pthread_setcancelstate(previous,NULL);
    return result;
}
static int klj_barrierattr_init(void *attribute){if(!attribute)return kl_errno_to_linux(EINVAL);unsigned flags=0;memcpy(attribute,&flags,sizeof(flags));return 0;}
static int klj_barrierattr_destroy(void *attribute){return klj_barrierattr_init(attribute);}
static int klj_barrierattr_getpshared(const void *attribute,int *shared){if(!attribute||!shared)return kl_errno_to_linux(EINVAL);unsigned flags;memcpy(&flags,attribute,sizeof(flags));*shared=flags&1;return 0;}
static int klj_barrierattr_setpshared(void *attribute,int shared){if(!attribute||(shared!=0&&shared!=1))return kl_errno_to_linux(EINVAL);unsigned flags;memcpy(&flags,attribute,sizeof(flags));flags=(flags&~1u)|(unsigned)shared;memcpy(attribute,&flags,sizeof(flags));return 0;}

// Bionic FORTIFY checks the caller's supplied bound, then retains strl*'s
// ordinary truncation/return-value semantics. Reuse the existing host functions
// and Klepton's diagnostic fatal path; never clamp an invalid object bound.
static size_t klj_strlcpy_chk(char *dst,const char *src,size_t size,size_t capacity){
    if(size>capacity)die("__strlcpy_chk destination buffer overflow");
    return strlcpy(dst,src,size);
}
static size_t klj_strlcat_chk(char *dst,const char *src,size_t size,size_t capacity){
    if(size>capacity)die("__strlcat_chk destination buffer overflow");
    return strlcat(dst,src,size);
}
static void *klj_native_import_lookup(const char *name){
#define KLJ_IMPORT(symbol,implementation) if(!strcmp(name,symbol))return (void *)implementation
    KLJ_IMPORT("pthread_barrier_init",klj_barrier_init);
    KLJ_IMPORT("pthread_barrier_destroy",klj_barrier_destroy);
    KLJ_IMPORT("pthread_barrier_wait",klj_barrier_wait);
    KLJ_IMPORT("pthread_barrierattr_init",klj_barrierattr_init);
    KLJ_IMPORT("pthread_barrierattr_destroy",klj_barrierattr_destroy);
    KLJ_IMPORT("pthread_barrierattr_getpshared",klj_barrierattr_getpshared);
    KLJ_IMPORT("pthread_barrierattr_setpshared",klj_barrierattr_setpshared);
    KLJ_IMPORT("__strlcpy_chk",klj_strlcpy_chk);
    KLJ_IMPORT("__strlcat_chk",klj_strlcat_chk);
#undef KLJ_IMPORT
    return NULL;
}
#endif
