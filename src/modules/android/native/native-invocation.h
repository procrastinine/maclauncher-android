#pragma once
#include "jni-invocation.h"
#include <setjmp.h>

/* Place the boundary immediately around original guest code. Native callers
 * keep their locks, cleanup records and returned allocations outside it. */
#define KLJ_INVOKE_GUEST(environment, statement) do { \
    JNIEnv *klj_call_env=(environment); \
    if(!klj_call_env){statement;} \
    else { \
        volatile KljJniInvocation klj_call_state; jmp_buf klj_call_exit; \
        KljJniInvocation *klj_call_scope=(KljJniInvocation *)&klj_call_state; \
        if(klj_jni_invocation_enter(klj_call_scope,klj_call_env)){ \
            klj_call_scope->exitContext=klj_call_exit; \
            if(!_setjmp(klj_call_exit)){statement;} \
        } \
        klj_jni_invocation_leave(klj_call_scope); \
    } \
} while(0)

JNIEnv *klj_guest_thread_environment(void);
