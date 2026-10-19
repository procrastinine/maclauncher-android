#pragma once
#include <pthread.h>

#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
#define KLJ_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#else
#define KLJ_RECURSIVE_MUTEX_INITIALIZER PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

typedef struct {pthread_mutex_t native;unsigned depth;int recursive;} KljForkLock;
#define KLJ_FORK_LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER,0,0}
#define KLJ_FORK_RECURSIVE_LOCK_INITIALIZER {KLJ_RECURSIVE_MUTEX_INITIALIZER,0,1}
static inline int klj_fork_lock(KljForkLock *lock){
    int error=pthread_mutex_lock(&lock->native);if(!error)++lock->depth;return error;
}
static inline int klj_fork_unlock(KljForkLock *lock){
    --lock->depth;int error=pthread_mutex_unlock(&lock->native);if(error)++lock->depth;return error;
}
// prepare owns one recursion level. Only that process's surviving thread uses
// the child's copy; inherited mutex owner IDs and parent waiters cannot be
// transferred. Preserve earlier recursion so live caller guards remain valid.
static inline int klj_fork_lock_finish(KljForkLock *lock,int child){
    if(!child)return klj_fork_unlock(lock);
    unsigned retained=lock->depth-1;
    pthread_mutex_t fresh=lock->recursive?(pthread_mutex_t)KLJ_RECURSIVE_MUTEX_INITIALIZER:
                                        (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    lock->native=fresh;lock->depth=0;
    while(retained--){int error=klj_fork_lock(lock);if(error)return error;}
    return 0;
}
