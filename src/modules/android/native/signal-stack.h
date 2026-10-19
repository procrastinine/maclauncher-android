#pragma once
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

/* AArch64 bionic orders flags before size; Darwin orders size before flags.
 * The caller owns the stack memory for the same lifetime on either platform. */
typedef struct {void *pointer;int32_t flags;uint32_t padding;uint64_t size;} klj_android_stack;
static int klj_sigaltstack(const klj_android_stack *input,klj_android_stack *previous){
    stack_t native={0},old={0};
    if(input){
        if(input->flags&~3){errno=EINVAL;return -1;}
        native.ss_sp=input->pointer;native.ss_size=input->size;
        native.ss_flags=((input->flags&1)?SS_ONSTACK:0)|((input->flags&2)?SS_DISABLE:0);
        /* Darwin checks size even when disabling; Linux ignores it then. */
        if((input->flags&2)&&native.ss_size<MINSIGSTKSZ)native.ss_size=MINSIGSTKSZ;
    }
    int result=sigaltstack(input?&native:NULL,previous?&old:NULL);
    if(!result&&previous){
        previous->pointer=old.ss_sp;previous->size=old.ss_size;previous->padding=0;
        previous->flags=((old.ss_flags&SS_ONSTACK)?1:0)|((old.ss_flags&SS_DISABLE)?2:0);
    }
    return result;
}
